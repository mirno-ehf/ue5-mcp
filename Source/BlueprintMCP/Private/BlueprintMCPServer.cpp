#include "BlueprintMCPServer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpPath.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/SavePackage.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "UObject/LinkerLoad.h"
#include "Engine/UserDefinedEnum.h"
#include "Editor.h"

// ============================================================
// Helpers
// ============================================================

FString FBlueprintMCPServer::JsonToString(TSharedRef<FJsonObject> JsonObj)
{
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(JsonObj, Writer);
	return Output;
}

FAssetData* FBlueprintMCPServer::FindBlueprintAsset(const FString& NameOrPath)
{
	for (FAssetData& Asset : AllBlueprintAssets)
	{
		if (Asset.AssetName.ToString() == NameOrPath || Asset.PackageName.ToString() == NameOrPath)
		{
			return &Asset;
		}
	}
	// Case-insensitive fallback
	for (FAssetData& Asset : AllBlueprintAssets)
	{
		if (Asset.AssetName.ToString().Equals(NameOrPath, ESearchCase::IgnoreCase) ||
			Asset.PackageName.ToString().Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			return &Asset;
		}
	}
	return nullptr;
}

FAssetData* FBlueprintMCPServer::FindMapAsset(const FString& NameOrPath)
{
	for (FAssetData& Asset : AllMapAssets)
	{
		if (Asset.AssetName.ToString() == NameOrPath || Asset.PackageName.ToString() == NameOrPath)
		{
			return &Asset;
		}
	}
	// Case-insensitive fallback
	for (FAssetData& Asset : AllMapAssets)
	{
		if (Asset.AssetName.ToString().Equals(NameOrPath, ESearchCase::IgnoreCase) ||
			Asset.PackageName.ToString().Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			return &Asset;
		}
	}
	return nullptr;
}

UBlueprint* FBlueprintMCPServer::LoadBlueprintByName(const FString& NameOrPath, FString& OutError)
{
	// Strategy 1: Try as a regular Blueprint asset
	FAssetData* Asset = FindBlueprintAsset(NameOrPath);
	if (Asset)
	{
		UBlueprint* BP = Cast<UBlueprint>(Asset->GetAsset());
		if (BP) return BP;
	}

	// Strategy 2: Try as a level blueprint (from a .umap)
	FAssetData* MapAsset = FindMapAsset(NameOrPath);
	if (MapAsset)
	{
		UWorld* World = Cast<UWorld>(MapAsset->GetAsset());
		if (World && World->PersistentLevel)
		{
			ULevelScriptBlueprint* LevelBP = World->PersistentLevel->GetLevelScriptBlueprint(true);
			if (LevelBP)
			{
				UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Loaded level blueprint from map '%s'"),
					*NameOrPath);
				return LevelBP;
			}
		}
		OutError = FString::Printf(TEXT("Map '%s' loaded but its level blueprint could not be retrieved. The map may not contain a level blueprint."), *NameOrPath);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Blueprint or map '%s' not found. Use list_blueprints to see available assets. Level blueprints are referenced by their map name (e.g. 'MAP_Ward')."), *NameOrPath);
	return nullptr;
}

TSharedPtr<FJsonObject> FBlueprintMCPServer::ParseBodyJson(const FString& Body)
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	FJsonSerializer::Deserialize(Reader, JsonObj);
	return JsonObj;
}

FString FBlueprintMCPServer::MakeErrorJson(const FString& Message)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("error"), Message);
	return JsonToString(E);
}

FString FBlueprintMCPServer::UrlDecode(const FString& EncodedString)
{
	FString Result;
	Result.Reserve(EncodedString.Len());

	for (int32 i = 0; i < EncodedString.Len(); ++i)
	{
		TCHAR C = EncodedString[i];
		if (C == TEXT('+'))
		{
			Result += TEXT(' ');
		}
		else if (C == TEXT('%') && i + 2 < EncodedString.Len())
		{
			FString HexStr = EncodedString.Mid(i + 1, 2);
			int32 HexVal = 0;
			bool bValid = true;
			for (TCHAR H : HexStr)
			{
				HexVal <<= 4;
				if (H >= TEXT('0') && H <= TEXT('9'))
					HexVal += H - TEXT('0');
				else if (H >= TEXT('a') && H <= TEXT('f'))
					HexVal += 10 + H - TEXT('a');
				else if (H >= TEXT('A') && H <= TEXT('F'))
					HexVal += 10 + H - TEXT('A');
				else
				{
					bValid = false;
					break;
				}
			}
			if (bValid)
			{
				Result += (TCHAR)HexVal;
				i += 2;
			}
			else
			{
				Result += C;
			}
		}
		else
		{
			Result += C;
		}
	}
	return Result;
}

UEdGraphNode* FBlueprintMCPServer::FindNodeByGuid(
	UBlueprint* BP, const FString& GuidString, UEdGraph** OutGraph)
{
	FGuid TargetGuid;
	FGuid::Parse(GuidString, TargetGuid);

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == TargetGuid)
			{
				if (OutGraph) *OutGraph = Graph;
				return Node;
			}
		}
	}
	return nullptr;
}

// ============================================================
// SEH wrappers for crash-safe compilation and saving.
// MSVC constraint: __try/__except functions must NOT contain C++
// objects with destructors. We factor the actual work into
// separate "inner" functions and only do try/except in thin wrappers.
// ============================================================
#if PLATFORM_WINDOWS

// Inner functions that do the actual C++ work (may have destructors)
static void CompileBlueprintInner(UBlueprint* BP, EBlueprintCompileOptions Opts)
{
	FKismetEditorUtilities::CompileBlueprint(BP, Opts, nullptr);
}

static ESavePackageResult SavePackageInner(
	UPackage* Package, UObject* Asset, const TCHAR* Filename,
	FSavePackageArgs* SaveArgs)
{
	FSavePackageResultStruct Result = UPackage::Save(Package, Asset, Filename, *SaveArgs);
	return Result.Result;
}

// SEH wrappers — absolutely NO C++ objects with destructors here.
// EXCEPTION_EXECUTE_HANDLER = 1 (avoiding Windows.h include)
#pragma warning(push)
#pragma warning(disable: 4611) // interaction between '_setjmp' and C++ object destruction
int32 TryCompileBlueprintSEH(UBlueprint* BP, EBlueprintCompileOptions Opts)
{
	__try
	{
		CompileBlueprintInner(BP, Opts);
		return 0;
	}
	__except (1)
	{
		return -1;
	}
}

static int32 TrySavePackageSEH(
	UPackage* Package, UObject* Asset, const TCHAR* Filename,
	FSavePackageArgs* SaveArgs, ESavePackageResult* OutResult)
{
	__try
	{
		*OutResult = SavePackageInner(Package, Asset, Filename, SaveArgs);
		return 0;
	}
	__except (1)
	{
		*OutResult = ESavePackageResult::Error;
		return -1;
	}
}

static void RefreshAllNodesInner(UBlueprint* BP)
{
	FBlueprintEditorUtils::RefreshAllNodes(BP);
}

int32 TryRefreshAllNodesSEH(UBlueprint* BP)
{
	__try
	{
		RefreshAllNodesInner(BP);
		return 0;
	}
	__except (1)
	{
		return -1;
	}
}
#pragma warning(pop)

#endif // PLATFORM_WINDOWS

// ============================================================
// Start / Stop / ProcessOneRequest
// ============================================================

bool FBlueprintMCPServer::Start(int32 InPort, bool bEditorMode)
{
	Port = InPort;
	bIsEditor = bEditorMode;

	// Scan asset registry
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Scanning asset registry..."));
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	ARM.Get().SearchAllAssets(true);
	ARM.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprintAssets, true);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Found %d Blueprint assets."), AllBlueprintAssets.Num());

	// Also scan for map assets (level blueprints live inside .umap packages)
	ARM.Get().GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), AllMapAssets, false);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Found %d Map assets (potential level blueprints)."), AllMapAssets.Num());

	// Start HTTP server
	FHttpServerModule& HttpModule = FModuleManager::LoadModuleChecked<FHttpServerModule>("HTTPServer");
	TSharedPtr<IHttpRouter> Router = HttpModule.GetHttpRouter(Port);
	if (!Router.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintMCP: Failed to create HTTP router on port %d"), Port);
		return false;
	}

	// Lambda that creates a queued handler — dispatches work to main thread
	auto QueuedHandler = [this](const FString& Endpoint)
	{
		return FHttpRequestHandler::CreateLambda(
			[this, Endpoint](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				TSharedPtr<FPendingRequest> Req = MakeShared<FPendingRequest>();
				Req->Endpoint = Endpoint;
				Req->QueryParams = Request.QueryParams;
				// Capture POST body as UTF-8 string
				if (Request.Body.Num() > 0)
				{
					TArray<uint8> NullTerminated(Request.Body);
					NullTerminated.Add(0);
					Req->Body = UTF8_TO_TCHAR((const ANSICHAR*)NullTerminated.GetData());
				}
				Req->OnComplete = OnComplete;
				RequestQueue.Enqueue(Req);
				return true;
			});
	};

	// /api/health — answered directly on HTTP thread (no asset access)
	Router->BindRoute(FHttpPath(TEXT("/api/health")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda(
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				J->SetStringField(TEXT("status"), TEXT("ok"));
				J->SetStringField(TEXT("mode"), bIsEditor ? TEXT("editor") : TEXT("commandlet"));
				J->SetNumberField(TEXT("blueprintCount"), AllBlueprintAssets.Num());
				J->SetNumberField(TEXT("mapCount"), AllMapAssets.Num());
				TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(
					JsonToString(J), TEXT("application/json"));
				OnComplete(MoveTemp(R));
				return true;
			}));

	// /api/shutdown — request graceful engine exit (commandlet only)
	Router->BindRoute(FHttpPath(TEXT("/api/shutdown")), EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda(
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
				if (bIsEditor)
				{
					J->SetStringField(TEXT("error"), TEXT("Cannot shut down the editor's MCP server."));
				}
				else
				{
					J->SetStringField(TEXT("status"), TEXT("shutting_down"));
					RequestEngineExit(TEXT("BlueprintMCP /api/shutdown"));
				}
				TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(
					JsonToString(J), TEXT("application/json"));
				OnComplete(MoveTemp(R));
				return true;
			}));

	// /api/list — answered directly (only reads immutable asset list)
	Router->BindRoute(FHttpPath(TEXT("/api/list")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda(
			[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
			{
				FString Resp = HandleList(Request.QueryParams);
				TUniquePtr<FHttpServerResponse> R = FHttpServerResponse::Create(
					Resp, TEXT("application/json"));
				OnComplete(MoveTemp(R));
				return true;
			}));

	// Queued (need main thread for LoadObject)
	Router->BindRoute(FHttpPath(TEXT("/api/blueprint")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("blueprint")));
	Router->BindRoute(FHttpPath(TEXT("/api/graph")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("graph")));
	Router->BindRoute(FHttpPath(TEXT("/api/search")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("search")));

	// Reference finder + write tools
	Router->BindRoute(FHttpPath(TEXT("/api/references")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("references")));
	Router->BindRoute(FHttpPath(TEXT("/api/replace-function-calls")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("replaceFunctionCalls")));
	Router->BindRoute(FHttpPath(TEXT("/api/change-variable-type")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("changeVariableType")));
	Router->BindRoute(FHttpPath(TEXT("/api/change-function-param-type")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("changeFunctionParamType")));
	Router->BindRoute(FHttpPath(TEXT("/api/delete-asset")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("deleteAsset")));
	Router->BindRoute(FHttpPath(TEXT("/api/test-save")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("testSave")));
	Router->BindRoute(FHttpPath(TEXT("/api/connect-pins")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("connectPins")));
	Router->BindRoute(FHttpPath(TEXT("/api/disconnect-pin")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("disconnectPin")));
	Router->BindRoute(FHttpPath(TEXT("/api/refresh-all-nodes")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("refreshAllNodes")));
	Router->BindRoute(FHttpPath(TEXT("/api/set-pin-default")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("setPinDefault")));
	Router->BindRoute(FHttpPath(TEXT("/api/move-node")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("moveNode")));
	Router->BindRoute(FHttpPath(TEXT("/api/get-node-comment")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("getNodeComment")));
	Router->BindRoute(FHttpPath(TEXT("/api/set-node-comment")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("setNodeComment")));
	Router->BindRoute(FHttpPath(TEXT("/api/get-pin-info")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("getPinInfo")));
	Router->BindRoute(FHttpPath(TEXT("/api/check-pin-compatibility")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("checkPinCompatibility")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-classes")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listClasses")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-functions")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listFunctions")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-properties")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listProperties")));
	Router->BindRoute(FHttpPath(TEXT("/api/change-struct-node-type")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("changeStructNodeType")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-function-parameter")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeFunctionParameter")));
	Router->BindRoute(FHttpPath(TEXT("/api/delete-node")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("deleteNode")));
	Router->BindRoute(FHttpPath(TEXT("/api/duplicate-nodes")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("duplicateNodes")));
	Router->BindRoute(FHttpPath(TEXT("/api/search-by-type")), EHttpServerRequestVerbs::VERB_GET,
		QueuedHandler(TEXT("searchByType")));
	Router->BindRoute(FHttpPath(TEXT("/api/validate-blueprint")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("validateBlueprint")));
	Router->BindRoute(FHttpPath(TEXT("/api/validate-all-blueprints")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("validateAllBlueprints")));
	Router->BindRoute(FHttpPath(TEXT("/api/add-node")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addNode")));
	Router->BindRoute(FHttpPath(TEXT("/api/rename-asset")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("renameAsset")));
	Router->BindRoute(FHttpPath(TEXT("/api/reparent-blueprint")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("reparentBlueprint")));
	Router->BindRoute(FHttpPath(TEXT("/api/set-blueprint-default")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("setBlueprintDefault")));
	Router->BindRoute(FHttpPath(TEXT("/api/create-blueprint")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("createBlueprint")));
	Router->BindRoute(FHttpPath(TEXT("/api/create-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("createGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/delete-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("deleteGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/rename-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("renameGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/add-variable")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addVariable")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-variable")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeVariable")));
	Router->BindRoute(FHttpPath(TEXT("/api/set-variable-metadata")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("setVariableMetadata")));

	// Interface tools
	Router->BindRoute(FHttpPath(TEXT("/api/add-interface")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addInterface")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-interface")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeInterface")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-interfaces")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listInterfaces")));

	// Event Dispatcher tools
	Router->BindRoute(FHttpPath(TEXT("/api/add-event-dispatcher")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addEventDispatcher")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-event-dispatchers")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listEventDispatchers")));

	// Function parameter tools
	Router->BindRoute(FHttpPath(TEXT("/api/add-function-parameter")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addFunctionParameter")));

	// Component tools
	Router->BindRoute(FHttpPath(TEXT("/api/add-component")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addComponent")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-component")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeComponent")));
	Router->BindRoute(FHttpPath(TEXT("/api/list-components")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("listComponents")));

	// Snapshot / Safety tools
	Router->BindRoute(FHttpPath(TEXT("/api/snapshot-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("snapshotGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/diff-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("diffGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/restore-graph")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("restoreGraph")));
	Router->BindRoute(FHttpPath(TEXT("/api/find-disconnected-pins")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("findDisconnectedPins")));
	Router->BindRoute(FHttpPath(TEXT("/api/analyze-rebuild-impact")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("analyzeRebuildImpact")));

	// Register TMap dispatch handlers
	RegisterHandlers();

	HttpModule.StartAllListeners();

	// Verify the listener actually bound by attempting a TCP connection
	bool bListenerReady = false;
	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
	{
		FSocket* TestSocket = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(NAME_Stream, TEXT("BlueprintMCP bind test"), false);
		if (TestSocket)
		{
			TSharedRef<FInternetAddr> Addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
			bool bIsValid = false;
			Addr->SetIp(TEXT("127.0.0.1"), bIsValid);
			Addr->SetPort(Port);
			bool bConnected = TestSocket->Connect(*Addr);
			TestSocket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TestSocket);

			if (bConnected)
			{
				bListenerReady = true;
				break;
			}
		}
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Bind check attempt %d/5 failed on port %d, retrying..."), Attempt + 1, Port);
		FPlatformProcess::Sleep(1.0f);
	}

	if (!bListenerReady)
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintMCP: Failed to bind HTTP listener on port %d. Port may be in use."), Port);
		HttpModule.StopAllListeners();
		return false;
	}

	bRunning = true;
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Server listening on http://localhost:%d"), Port);
	return true;
}

void FBlueprintMCPServer::Stop()
{
	if (!bRunning) return;

	FHttpServerModule& HttpModule = FModuleManager::LoadModuleChecked<FHttpServerModule>("HTTPServer");
	HttpModule.StopAllListeners();
	bRunning = false;
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Server stopped."));
}

bool FBlueprintMCPServer::ProcessOneRequest()
{
	TSharedPtr<FPendingRequest> Req;
	if (!RequestQueue.Dequeue(Req))
	{
		return false;
	}

	FString Response;
	if (FRequestHandler* Handler = HandlerMap.Find(Req->Endpoint))
	{
		// Wrap mutation endpoints in an undo transaction so users can Ctrl+Z
		const bool bIsMutation = MutationEndpoints.Contains(Req->Endpoint);
		if (bIsMutation && GEditor)
		{
			GEditor->BeginTransaction(FText::FromString(FString::Printf(TEXT("BlueprintMCP: %s"), *Req->Endpoint)));
		}

		Response = (*Handler)(Req->QueryParams, Req->Body);

		if (bIsMutation && GEditor)
		{
			GEditor->EndTransaction();
		}
	}
	else
	{
		Response = MakeErrorJson(FString::Printf(TEXT("Unknown endpoint: %s"), *Req->Endpoint));
	}

	// Send the response back via the HTTP callback (non-blocking)
	TUniquePtr<FHttpServerResponse> HttpResp = FHttpServerResponse::Create(
		Response, TEXT("application/json"));
	Req->OnComplete(MoveTemp(HttpResp));

	return true;
}

void FBlueprintMCPServer::RegisterHandlers()
{
	// Mutation endpoints — wrapped in undo transactions by ProcessOneRequest()
	MutationEndpoints = {
		TEXT("replaceFunctionCalls"),
		TEXT("changeVariableType"),
		TEXT("changeFunctionParamType"),
		TEXT("removeFunctionParameter"),
		TEXT("deleteAsset"),
		TEXT("connectPins"),
		TEXT("disconnectPin"),
		TEXT("refreshAllNodes"),
		TEXT("setPinDefault"),
		TEXT("moveNode"),
		TEXT("changeStructNodeType"),
		TEXT("deleteNode"),
		TEXT("duplicateNodes"),
		TEXT("addNode"),
		TEXT("setNodeComment"),
		TEXT("renameAsset"),
		TEXT("reparentBlueprint"),
		TEXT("setBlueprintDefault"),
		TEXT("createBlueprint"),
		TEXT("createGraph"),
		TEXT("deleteGraph"),
		TEXT("renameGraph"),
		TEXT("addVariable"),
		TEXT("removeVariable"),
		TEXT("setVariableMetadata"),
		TEXT("addInterface"),
		TEXT("removeInterface"),
		TEXT("addEventDispatcher"),
		TEXT("addFunctionParameter"),
		TEXT("addComponent"),
		TEXT("removeComponent"),
		TEXT("restoreGraph"),
	};

	// GET handlers (use QueryParams, ignore Body)
	HandlerMap.Add(TEXT("blueprint"),         [this](const TMap<FString, FString>& P, const FString&) { return HandleGetBlueprint(P); });
	HandlerMap.Add(TEXT("graph"),             [this](const TMap<FString, FString>& P, const FString&) { return HandleGetGraph(P); });
	HandlerMap.Add(TEXT("search"),            [this](const TMap<FString, FString>& P, const FString&) { return HandleSearch(P); });
	HandlerMap.Add(TEXT("references"),        [this](const TMap<FString, FString>& P, const FString&) { return HandleFindReferences(P); });
	HandlerMap.Add(TEXT("testSave"),          [this](const TMap<FString, FString>& P, const FString&) { return HandleTestSave(P); });
	HandlerMap.Add(TEXT("searchByType"),      [this](const TMap<FString, FString>& P, const FString&) { return HandleSearchByType(P); });

	// POST handlers (use Body, ignore QueryParams)
	HandlerMap.Add(TEXT("replaceFunctionCalls"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleReplaceFunctionCalls(B); });
	HandlerMap.Add(TEXT("changeVariableType"),      [this](const TMap<FString, FString>&, const FString& B) { return HandleChangeVariableType(B); });
	HandlerMap.Add(TEXT("changeFunctionParamType"), [this](const TMap<FString, FString>&, const FString& B) { return HandleChangeFunctionParamType(B); });
	HandlerMap.Add(TEXT("removeFunctionParameter"), [this](const TMap<FString, FString>&, const FString& B) { return HandleRemoveFunctionParameter(B); });
	HandlerMap.Add(TEXT("deleteAsset"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleDeleteAsset(B); });
	HandlerMap.Add(TEXT("connectPins"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleConnectPins(B); });
	HandlerMap.Add(TEXT("disconnectPin"),           [this](const TMap<FString, FString>&, const FString& B) { return HandleDisconnectPin(B); });
	HandlerMap.Add(TEXT("refreshAllNodes"),         [this](const TMap<FString, FString>&, const FString& B) { return HandleRefreshAllNodes(B); });
	HandlerMap.Add(TEXT("setPinDefault"),           [this](const TMap<FString, FString>&, const FString& B) { return HandleSetPinDefault(B); });
	HandlerMap.Add(TEXT("moveNode"),               [this](const TMap<FString, FString>&, const FString& B) { return HandleMoveNode(B); });
	HandlerMap.Add(TEXT("getNodeComment"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleGetNodeComment(B); });
	HandlerMap.Add(TEXT("setNodeComment"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleSetNodeComment(B); });
	HandlerMap.Add(TEXT("getPinInfo"),              [this](const TMap<FString, FString>&, const FString& B) { return HandleGetPinInfo(B); });
	HandlerMap.Add(TEXT("checkPinCompatibility"),   [this](const TMap<FString, FString>&, const FString& B) { return HandleCheckPinCompatibility(B); });
	HandlerMap.Add(TEXT("listClasses"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleListClasses(B); });
	HandlerMap.Add(TEXT("listFunctions"),           [this](const TMap<FString, FString>&, const FString& B) { return HandleListFunctions(B); });
	HandlerMap.Add(TEXT("listProperties"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleListProperties(B); });
	HandlerMap.Add(TEXT("changeStructNodeType"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleChangeStructNodeType(B); });
	HandlerMap.Add(TEXT("deleteNode"),              [this](const TMap<FString, FString>&, const FString& B) { return HandleDeleteNode(B); });
	HandlerMap.Add(TEXT("duplicateNodes"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleDuplicateNodes(B); });
	HandlerMap.Add(TEXT("validateBlueprint"),       [this](const TMap<FString, FString>&, const FString& B) { return HandleValidateBlueprint(B); });
	HandlerMap.Add(TEXT("validateAllBlueprints"),   [this](const TMap<FString, FString>&, const FString& B) { return HandleValidateAllBlueprints(B); });
	HandlerMap.Add(TEXT("addNode"),                 [this](const TMap<FString, FString>&, const FString& B) { return HandleAddNode(B); });
	HandlerMap.Add(TEXT("renameAsset"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleRenameAsset(B); });
	HandlerMap.Add(TEXT("reparentBlueprint"),       [this](const TMap<FString, FString>&, const FString& B) { return HandleReparentBlueprint(B); });
	HandlerMap.Add(TEXT("setBlueprintDefault"),     [this](const TMap<FString, FString>&, const FString& B) { return HandleSetBlueprintDefault(B); });
	HandlerMap.Add(TEXT("createBlueprint"),         [this](const TMap<FString, FString>&, const FString& B) { return HandleCreateBlueprint(B); });
	HandlerMap.Add(TEXT("createGraph"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleCreateGraph(B); });
	HandlerMap.Add(TEXT("deleteGraph"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleDeleteGraph(B); });
	HandlerMap.Add(TEXT("renameGraph"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleRenameGraph(B); });
	HandlerMap.Add(TEXT("addVariable"),             [this](const TMap<FString, FString>&, const FString& B) { return HandleAddVariable(B); });
	HandlerMap.Add(TEXT("removeVariable"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleRemoveVariable(B); });
	HandlerMap.Add(TEXT("setVariableMetadata"),     [this](const TMap<FString, FString>&, const FString& B) { return HandleSetVariableMetadata(B); });
	HandlerMap.Add(TEXT("addInterface"),            [this](const TMap<FString, FString>&, const FString& B) { return HandleAddInterface(B); });
	HandlerMap.Add(TEXT("removeInterface"),         [this](const TMap<FString, FString>&, const FString& B) { return HandleRemoveInterface(B); });
	HandlerMap.Add(TEXT("listInterfaces"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleListInterfaces(B); });
	HandlerMap.Add(TEXT("addEventDispatcher"),      [this](const TMap<FString, FString>&, const FString& B) { return HandleAddEventDispatcher(B); });
	HandlerMap.Add(TEXT("listEventDispatchers"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleListEventDispatchers(B); });
	HandlerMap.Add(TEXT("addFunctionParameter"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleAddFunctionParameter(B); });
	HandlerMap.Add(TEXT("addComponent"),            [this](const TMap<FString, FString>&, const FString& B) { return HandleAddComponent(B); });
	HandlerMap.Add(TEXT("removeComponent"),         [this](const TMap<FString, FString>&, const FString& B) { return HandleRemoveComponent(B); });
	HandlerMap.Add(TEXT("listComponents"),          [this](const TMap<FString, FString>&, const FString& B) { return HandleListComponents(B); });
	HandlerMap.Add(TEXT("snapshotGraph"),           [this](const TMap<FString, FString>&, const FString& B) { return HandleSnapshotGraph(B); });
	HandlerMap.Add(TEXT("diffGraph"),               [this](const TMap<FString, FString>&, const FString& B) { return HandleDiffGraph(B); });
	HandlerMap.Add(TEXT("restoreGraph"),            [this](const TMap<FString, FString>&, const FString& B) { return HandleRestoreGraph(B); });
	HandlerMap.Add(TEXT("findDisconnectedPins"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleFindDisconnectedPins(B); });
	HandlerMap.Add(TEXT("analyzeRebuildImpact"),    [this](const TMap<FString, FString>&, const FString& B) { return HandleAnalyzeRebuildImpact(B); });
}

// ============================================================
// SaveBlueprintPackage
// ============================================================

bool FBlueprintMCPServer::SaveBlueprintPackage(UBlueprint* BP)
{
	UPackage* Package = BP->GetPackage();
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: SaveBlueprintPackage — begin for '%s'"), *BP->GetName());

	// 1. Build absolute package filename — use .umap for map packages, .uasset otherwise
	FString PackageExtension = Package->ContainsMap()
		? FPackageName::GetMapPackageExtension()
		: FPackageName::GetAssetPackageExtension();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), PackageExtension);
	PackageFilename = FPaths::ConvertRelativePathToFull(PackageFilename);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Save target: %s"), *PackageFilename);

	// 2. Phase 1: Try explicit compilation (same flags as UCompileAllBlueprintsCommandlet)
	bool bCompiled = false;
	{
		EBlueprintCompileOptions CompileOpts =
			EBlueprintCompileOptions::SkipSave |
			EBlueprintCompileOptions::BatchCompile |
			EBlueprintCompileOptions::SkipGarbageCollection |
			EBlueprintCompileOptions::SkipFiBSearchMetaUpdate;

		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Phase 1: Attempting explicit compilation..."));

#if PLATFORM_WINDOWS
		int32 CompileResult = TryCompileBlueprintSEH(BP, CompileOpts);
		if (CompileResult == 0)
		{
			bCompiled = (BP->Status == BS_UpToDate);
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Compilation %s (status=%d)"),
				bCompiled ? TEXT("succeeded") : TEXT("completed with warnings"), (int32)BP->Status);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP:   Compilation crashed (SEH), proceeding uncompiled"));
		}
#else
		FKismetEditorUtilities::CompileBlueprint(BP, CompileOpts, nullptr);
		bCompiled = (BP->Status == BS_UpToDate);
#endif
	}

	// 3. Phase 2: Set guards for save
	uint8 OldRegen = BP->bIsRegeneratingOnLoad;
	BP->bIsRegeneratingOnLoad = true;

	EBlueprintStatus OldStatus = (EBlueprintStatus)(uint8)BP->Status;
	if (!bCompiled)
	{
		// Tell PreSave the BP is up-to-date so it doesn't try to compile
		BP->Status = BS_UpToDate;
	}

	// 4. Clear read-only attribute if present (source control or LFS may set this)
	if (FPlatformFileManager::Get().GetPlatformFile().IsReadOnly(*PackageFilename))
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Clearing read-only attribute on %s"), *PackageFilename);
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*PackageFilename, false);
	}

	// 5. Phase 3: Save with SAVE_NoError + SEH protection
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;

	// For level blueprints (map packages), the base object should be the UWorld, not the BP
	bool bIsMapPackage = Package->ContainsMap();
	UObject* BaseObject = BP;
	if (bIsMapPackage)
	{
		// Find the UWorld in this package — it's the actual asset for .umap files
		UWorld* World = FindObject<UWorld>(Package, *Package->GetName().Mid(Package->GetName().Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd) + 1));
		if (!World)
		{
			// Fallback: iterate the package to find any UWorld
			ForEachObjectWithPackage(Package, [&World](UObject* Obj) {
				if (UWorld* W = Cast<UWorld>(Obj))
				{
					World = W;
					return false; // stop
				}
				return true; // continue
			});
		}
		if (World)
		{
			BaseObject = World;
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Map package detected — saving UWorld '%s'"), *World->GetName());
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP:   Map package detected but no UWorld found — saving with BP as base"));
		}
	}

	ESavePackageResult SaveResult = ESavePackageResult::Error;

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP:   Phase 3: Calling UPackage::Save (compiled=%s, isMap=%s)..."),
		bCompiled ? TEXT("yes") : TEXT("no"), bIsMapPackage ? TEXT("yes") : TEXT("no"));

#if PLATFORM_WINDOWS
	int32 SEHCode = TrySavePackageSEH(Package, BaseObject, *PackageFilename, &SaveArgs, &SaveResult);
	if (SEHCode != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("BlueprintMCP:   UPackage::Save CRASHED (SEH exception caught)"));
	}
#else
	FSavePackageResultStruct Result = UPackage::Save(Package, BaseObject, *PackageFilename, SaveArgs);
	SaveResult = Result.Result;
#endif

	// 6. Restore guards
	BP->bIsRegeneratingOnLoad = OldRegen;
	if (!bCompiled)
	{
		BP->Status = (TEnumAsByte<EBlueprintStatus>)OldStatus;
	}

	bool bSuccess = (SaveResult == ESavePackageResult::Success);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: SaveBlueprintPackage — %s for '%s' (compiled=%s, result=%d)"),
		bSuccess ? TEXT("SUCCEEDED") : TEXT("FAILED"),
		*BP->GetName(), bCompiled ? TEXT("yes") : TEXT("no"), (int32)SaveResult);

	return bSuccess;
}

// ============================================================
// Blueprint serialization (graphs / nodes / pins)
// ============================================================

TSharedRef<FJsonObject> FBlueprintMCPServer::SerializeBlueprint(UBlueprint* BP)
{
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("name"), BP->GetName());
	J->SetStringField(TEXT("path"), BP->GetPackage()->GetName());
	J->SetStringField(TEXT("parentClass"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None"));
	J->SetStringField(TEXT("blueprintType"),
		StaticEnum<EBlueprintType>()->GetNameStringByValue((int64)BP->BlueprintType));

	// Variables
	TArray<TSharedPtr<FJsonValue>> Vars;
	for (const FBPVariableDescription& V : BP->NewVariables)
	{
		TSharedRef<FJsonObject> VJ = MakeShared<FJsonObject>();
		VJ->SetStringField(TEXT("name"), V.VarName.ToString());
		VJ->SetStringField(TEXT("type"), V.VarType.PinCategory.ToString());
		if (V.VarType.PinSubCategoryObject.IsValid())
			VJ->SetStringField(TEXT("subtype"), V.VarType.PinSubCategoryObject->GetName());
		VJ->SetBoolField(TEXT("isArray"), V.VarType.IsArray());
		VJ->SetBoolField(TEXT("isSet"), V.VarType.IsSet());
		VJ->SetBoolField(TEXT("isMap"), V.VarType.IsMap());
		VJ->SetStringField(TEXT("category"), V.Category.ToString());
		VJ->SetStringField(TEXT("defaultValue"), V.DefaultValue);
		Vars.Add(MakeShared<FJsonValueObject>(VJ));
	}
	J->SetArrayField(TEXT("variables"), Vars);

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> Ifaces;
	for (const FBPInterfaceDescription& I : BP->ImplementedInterfaces)
	{
		if (I.Interface)
			Ifaces.Add(MakeShared<FJsonValueString>(I.Interface->GetName()));
	}
	J->SetArrayField(TEXT("interfaces"), Ifaces);

	// Graphs
	TArray<TSharedPtr<FJsonValue>> GraphArr;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		TSharedPtr<FJsonObject> GJ = SerializeGraph(Graph);
		if (GJ.IsValid())
			GraphArr.Add(MakeShared<FJsonValueObject>(GJ.ToSharedRef()));
	}
	J->SetArrayField(TEXT("graphs"), GraphArr);
	return J;
}

TSharedPtr<FJsonObject> FBlueprintMCPServer::SerializeGraph(UEdGraph* Graph)
{
	TSharedRef<FJsonObject> GJ = MakeShared<FJsonObject>();
	GJ->SetStringField(TEXT("name"), Graph->GetName());
	GJ->SetStringField(TEXT("schema"), Graph->Schema ? Graph->Schema->GetClass()->GetName() : TEXT("Unknown"));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		TSharedPtr<FJsonObject> NJ = SerializeNode(Node);
		if (NJ.IsValid())
			Nodes.Add(MakeShared<FJsonValueObject>(NJ.ToSharedRef()));
	}
	GJ->SetArrayField(TEXT("nodes"), Nodes);
	return GJ;
}

TSharedPtr<FJsonObject> FBlueprintMCPServer::SerializeNode(UEdGraphNode* Node)
{
	TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
	NJ->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
	NJ->SetStringField(TEXT("class"), Node->GetClass()->GetName());
	NJ->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	if (!Node->NodeComment.IsEmpty())
		NJ->SetStringField(TEXT("comment"), Node->NodeComment);
	NJ->SetNumberField(TEXT("posX"), Node->NodePosX);
	NJ->SetNumberField(TEXT("posY"), Node->NodePosY);

	// K2Node specifics — check CallParentFunction before CallFunction (inheritance)
	if (auto* CPF = Cast<UK2Node_CallParentFunction>(Node))
	{
		NJ->SetStringField(TEXT("functionName"), CPF->FunctionReference.GetMemberName().ToString());
		if (CPF->FunctionReference.GetMemberParentClass())
			NJ->SetStringField(TEXT("targetClass"), CPF->FunctionReference.GetMemberParentClass()->GetName());
		NJ->SetStringField(TEXT("nodeType"), TEXT("CallParentFunction"));
	}
	else if (auto* CF = Cast<UK2Node_CallFunction>(Node))
	{
		NJ->SetStringField(TEXT("functionName"), CF->FunctionReference.GetMemberName().ToString());
		if (CF->FunctionReference.GetMemberParentClass())
			NJ->SetStringField(TEXT("targetClass"), CF->FunctionReference.GetMemberParentClass()->GetName());
	}
	else if (auto* FE = Cast<UK2Node_FunctionEntry>(Node))
	{
		NJ->SetStringField(TEXT("nodeType"), TEXT("FunctionEntry"));

		// Serialize UserDefinedPins (parameter names and types)
		TArray<TSharedPtr<FJsonValue>> ParamArr;
		for (const TSharedPtr<FUserPinInfo>& PinInfo : FE->UserDefinedPins)
		{
			if (!PinInfo.IsValid()) continue;
			TSharedRef<FJsonObject> ParamJ = MakeShared<FJsonObject>();
			ParamJ->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
			FString ParamType = PinInfo->PinType.PinCategory.ToString();
			ParamJ->SetStringField(TEXT("type"), ParamType);
			if (PinInfo->PinType.PinSubCategoryObject.IsValid())
				ParamJ->SetStringField(TEXT("subtype"), PinInfo->PinType.PinSubCategoryObject->GetName());
			else if (ParamType == TEXT("None") || ParamType.IsEmpty())
				ParamJ->SetBoolField(TEXT("typeUnknown"), true);
			ParamArr.Add(MakeShared<FJsonValueObject>(ParamJ));
		}
		NJ->SetArrayField(TEXT("parameters"), ParamArr);
	}
	else if (auto* Ev = Cast<UK2Node_Event>(Node))
	{
		NJ->SetStringField(TEXT("eventName"), Ev->EventReference.GetMemberName().ToString());
		NJ->SetStringField(TEXT("nodeType"), Ev->bOverrideFunction ? TEXT("OverrideEvent") : TEXT("Event"));
	}
	else if (auto* CE = Cast<UK2Node_CustomEvent>(Node))
	{
		NJ->SetStringField(TEXT("eventName"), CE->CustomFunctionName.ToString());
		NJ->SetStringField(TEXT("nodeType"), TEXT("CustomEvent"));

		// Serialize UserDefinedPins (parameter names and types)
		TArray<TSharedPtr<FJsonValue>> ParamArr;
		for (const TSharedPtr<FUserPinInfo>& PinInfo : CE->UserDefinedPins)
		{
			if (!PinInfo.IsValid()) continue;
			TSharedRef<FJsonObject> ParamJ = MakeShared<FJsonObject>();
			ParamJ->SetStringField(TEXT("name"), PinInfo->PinName.ToString());
			FString ParamType = PinInfo->PinType.PinCategory.ToString();
			ParamJ->SetStringField(TEXT("type"), ParamType);
			if (PinInfo->PinType.PinSubCategoryObject.IsValid())
				ParamJ->SetStringField(TEXT("subtype"), PinInfo->PinType.PinSubCategoryObject->GetName());
			else if (ParamType == TEXT("None") || ParamType.IsEmpty())
				ParamJ->SetBoolField(TEXT("typeUnknown"), true);
			ParamArr.Add(MakeShared<FJsonValueObject>(ParamJ));
		}
		NJ->SetArrayField(TEXT("parameters"), ParamArr);
	}
	else if (auto* VG = Cast<UK2Node_VariableGet>(Node))
	{
		NJ->SetStringField(TEXT("variableName"), VG->GetVarName().ToString());
		NJ->SetStringField(TEXT("nodeType"), TEXT("VariableGet"));
	}
	else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
	{
		NJ->SetStringField(TEXT("variableName"), VS->GetVarName().ToString());
		NJ->SetStringField(TEXT("nodeType"), TEXT("VariableSet"));
	}
	else if (auto* MI = Cast<UK2Node_MacroInstance>(Node))
	{
		if (MI->GetMacroGraph())
			NJ->SetStringField(TEXT("macroName"), MI->GetMacroGraph()->GetName());
		NJ->SetStringField(TEXT("nodeType"), TEXT("MacroInstance"));
	}
	else if (auto* DC = Cast<UK2Node_DynamicCast>(Node))
	{
		if (DC->TargetType)
			NJ->SetStringField(TEXT("castTarget"), DC->TargetType->GetName());
		NJ->SetStringField(TEXT("nodeType"), TEXT("DynamicCast"));
	}
	else if (Cast<UK2Node_IfThenElse>(Node))
	{
		NJ->SetStringField(TEXT("nodeType"), TEXT("Branch"));
	}

	// Pins
	TArray<TSharedPtr<FJsonValue>> Pins;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden) continue;
		TSharedPtr<FJsonObject> PJ = SerializePin(Pin);
		if (PJ.IsValid())
			Pins.Add(MakeShared<FJsonValueObject>(PJ.ToSharedRef()));
	}
	NJ->SetArrayField(TEXT("pins"), Pins);
	return NJ;
}

TSharedPtr<FJsonObject> FBlueprintMCPServer::SerializePin(UEdGraphPin* Pin)
{
	TSharedRef<FJsonObject> PJ = MakeShared<FJsonObject>();
	PJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
	PJ->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
	PJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	if (Pin->PinType.PinSubCategoryObject.IsValid())
		PJ->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategoryObject->GetName());
	if (!Pin->DefaultValue.IsEmpty())
		PJ->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);

	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Conns;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
			CJ->SetStringField(TEXT("nodeId"), Linked->GetOwningNode()->NodeGuid.ToString());
			CJ->SetStringField(TEXT("pinName"), Linked->PinName.ToString());
			Conns.Add(MakeShared<FJsonValueObject>(CJ));
		}
		PJ->SetArrayField(TEXT("connections"), Conns);
	}
	return PJ;
}

// ============================================================
// ResolveTypeFromString — shared type resolution helper
// ============================================================

bool FBlueprintMCPServer::ResolveTypeFromString(
	const FString& TypeName, FEdGraphPinType& OutPinType, FString& OutError)
{
	FString TypeLower = TypeName.ToLower();

	if (TypeLower == TEXT("bool") || TypeLower == TEXT("boolean"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeLower == TEXT("int") || TypeLower == TEXT("int32") || TypeLower == TEXT("integer"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeLower == TEXT("int64"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (TypeLower == TEXT("float") || TypeLower == TEXT("double") || TypeLower == TEXT("real"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = TEXT("double");
	}
	else if (TypeLower == TEXT("string"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeLower == TEXT("name"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeLower == TEXT("text"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (TypeLower == TEXT("byte"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	}
	else if (TypeLower == TEXT("vector") || TypeLower == TEXT("fvector"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeLower == TEXT("rotator") || TypeLower == TEXT("frotator"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeLower == TEXT("transform") || TypeLower == TEXT("ftransform"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (TypeLower == TEXT("linearcolor") || TypeLower == TEXT("flinearcolor"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (TypeLower == TEXT("vector2d") || TypeLower == TEXT("fvector2d"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
	}
	else if (TypeLower == TEXT("object"))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		OutPinType.PinSubCategoryObject = UObject::StaticClass();
	}
	else
	{
		// Try as a struct (F-prefix or raw name)
		FString InternalName = TypeName;
		bool bTriedAsStruct = false;

		if (TypeName.StartsWith(TEXT("F")) || TypeName.StartsWith(TEXT("S_")) || (!TypeName.StartsWith(TEXT("E"))))
		{
			if (TypeName.StartsWith(TEXT("F")))
			{
				InternalName = TypeName.Mid(1);
			}

			UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*InternalName);
			if (!FoundStruct)
			{
				for (TObjectIterator<UScriptStruct> It; It; ++It)
				{
					if (It->GetName() == InternalName || It->GetName() == TypeName)
					{
						FoundStruct = *It;
						break;
					}
				}
			}

			if (FoundStruct)
			{
				OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutPinType.PinSubCategoryObject = FoundStruct;
				bTriedAsStruct = true;
			}
		}

		if (!bTriedAsStruct)
		{
			// Try as an enum (E-prefix or raw name)
			FString EnumInternalName = TypeName;
			if (TypeName.StartsWith(TEXT("E")))
			{
				EnumInternalName = TypeName.Mid(1);
			}

			UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumInternalName);
			if (!FoundEnum)
			{
				for (TObjectIterator<UEnum> It; It; ++It)
				{
					if (It->GetName() == EnumInternalName || It->GetName() == TypeName)
					{
						FoundEnum = *It;
						break;
					}
				}
			}

			if (FoundEnum)
			{
				if (FoundEnum->GetCppForm() == UEnum::ECppForm::EnumClass)
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
				}
				else
				{
					OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				}
				OutPinType.PinSubCategoryObject = FoundEnum;
			}
			else
			{
				OutError = FString::Printf(
					TEXT("Unknown type '%s'. Use: bool, int, float, string, name, text, byte, vector, rotator, transform, or a struct/enum name (e.g. FVector, EMyEnum)"),
					*TypeName);
				return false;
			}
		}
	}

	return true;
}
