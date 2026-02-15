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
		OutError = FString::Printf(TEXT("Map '%s' loaded but level blueprint could not be retrieved"), *NameOrPath);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Blueprint or map '%s' not found"), *NameOrPath);
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
static int32 TryCompileBlueprintSEH(UBlueprint* BP, EBlueprintCompileOptions Opts)
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

static int32 TryRefreshAllNodesSEH(UBlueprint* BP)
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
	Router->BindRoute(FHttpPath(TEXT("/api/change-struct-node-type")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("changeStructNodeType")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-function-parameter")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeFunctionParameter")));
	Router->BindRoute(FHttpPath(TEXT("/api/delete-node")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("deleteNode")));
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
	Router->BindRoute(FHttpPath(TEXT("/api/add-variable")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("addVariable")));
	Router->BindRoute(FHttpPath(TEXT("/api/remove-variable")), EHttpServerRequestVerbs::VERB_POST,
		QueuedHandler(TEXT("removeVariable")));

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
	if (Req->Endpoint == TEXT("blueprint"))
	{
		Response = HandleGetBlueprint(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("graph"))
	{
		Response = HandleGetGraph(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("search"))
	{
		Response = HandleSearch(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("references"))
	{
		Response = HandleFindReferences(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("replaceFunctionCalls"))
	{
		Response = HandleReplaceFunctionCalls(Req->Body);
	}
	else if (Req->Endpoint == TEXT("changeVariableType"))
	{
		Response = HandleChangeVariableType(Req->Body);
	}
	else if (Req->Endpoint == TEXT("changeFunctionParamType"))
	{
		Response = HandleChangeFunctionParamType(Req->Body);
	}
	else if (Req->Endpoint == TEXT("deleteAsset"))
	{
		Response = HandleDeleteAsset(Req->Body);
	}
	else if (Req->Endpoint == TEXT("testSave"))
	{
		Response = HandleTestSave(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("connectPins"))
	{
		Response = HandleConnectPins(Req->Body);
	}
	else if (Req->Endpoint == TEXT("disconnectPin"))
	{
		Response = HandleDisconnectPin(Req->Body);
	}
	else if (Req->Endpoint == TEXT("refreshAllNodes"))
	{
		Response = HandleRefreshAllNodes(Req->Body);
	}
	else if (Req->Endpoint == TEXT("setPinDefault"))
	{
		Response = HandleSetPinDefault(Req->Body);
	}
	else if (Req->Endpoint == TEXT("changeStructNodeType"))
	{
		Response = HandleChangeStructNodeType(Req->Body);
	}
	else if (Req->Endpoint == TEXT("removeFunctionParameter"))
	{
		Response = HandleRemoveFunctionParameter(Req->Body);
	}
	else if (Req->Endpoint == TEXT("deleteNode"))
	{
		Response = HandleDeleteNode(Req->Body);
	}
	else if (Req->Endpoint == TEXT("searchByType"))
	{
		Response = HandleSearchByType(Req->QueryParams);
	}
	else if (Req->Endpoint == TEXT("validateBlueprint"))
	{
		Response = HandleValidateBlueprint(Req->Body);
	}
	else if (Req->Endpoint == TEXT("validateAllBlueprints"))
	{
		Response = HandleValidateAllBlueprints(Req->Body);
	}
	else if (Req->Endpoint == TEXT("addNode"))
	{
		Response = HandleAddNode(Req->Body);
	}
	else if (Req->Endpoint == TEXT("renameAsset"))
	{
		Response = HandleRenameAsset(Req->Body);
	}
	else if (Req->Endpoint == TEXT("reparentBlueprint"))
	{
		Response = HandleReparentBlueprint(Req->Body);
	}
	else if (Req->Endpoint == TEXT("setBlueprintDefault"))
	{
		Response = HandleSetBlueprintDefault(Req->Body);
	}
	else if (Req->Endpoint == TEXT("createBlueprint"))
	{
		Response = HandleCreateBlueprint(Req->Body);
	}
	else if (Req->Endpoint == TEXT("createGraph"))
	{
		Response = HandleCreateGraph(Req->Body);
	}
	else if (Req->Endpoint == TEXT("addVariable"))
	{
		Response = HandleAddVariable(Req->Body);
	}
	else if (Req->Endpoint == TEXT("removeVariable"))
	{
		Response = HandleRemoveVariable(Req->Body);
	}
	else if (Req->Endpoint == TEXT("snapshotGraph"))
	{
		Response = HandleSnapshotGraph(Req->Body);
	}
	else if (Req->Endpoint == TEXT("diffGraph"))
	{
		Response = HandleDiffGraph(Req->Body);
	}
	else if (Req->Endpoint == TEXT("restoreGraph"))
	{
		Response = HandleRestoreGraph(Req->Body);
	}
	else if (Req->Endpoint == TEXT("findDisconnectedPins"))
	{
		Response = HandleFindDisconnectedPins(Req->Body);
	}
	else if (Req->Endpoint == TEXT("analyzeRebuildImpact"))
	{
		Response = HandleAnalyzeRebuildImpact(Req->Body);
	}

	// Send the response back via the HTTP callback (non-blocking)
	TUniquePtr<FHttpServerResponse> HttpResp = FHttpServerResponse::Create(
		Response, TEXT("application/json"));
	Req->OnComplete(MoveTemp(HttpResp));

	return true;
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
// Request handlers
// ============================================================

FString FBlueprintMCPServer::HandleList(const TMap<FString, FString>& Params)
{
	const FString* Filter = Params.Find(TEXT("filter"));
	const FString* ParentClassFilter = Params.Find(TEXT("parentClass"));

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.PackageName.ToString();

		if (Filter && !Filter->IsEmpty())
		{
			if (!Name.Contains(*Filter, ESearchCase::IgnoreCase) &&
				!Path.Contains(*Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FString ParentClass;
		Asset.GetTagValue(FName(TEXT("ParentClass")), ParentClass);
		// Tag stores full path — extract short name
		int32 DotIndex;
		if (ParentClass.FindLastChar('.', DotIndex))
		{
			ParentClass = ParentClass.Mid(DotIndex + 1);
		}

		if (ParentClassFilter && !ParentClassFilter->IsEmpty())
		{
			if (!ParentClass.Contains(*ParentClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("parentClass"), ParentClass);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Also include level blueprints from maps
	for (const FAssetData& Asset : AllMapAssets)
	{
		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.PackageName.ToString();

		if (Filter && !Filter->IsEmpty())
		{
			if (!Name.Contains(*Filter, ESearchCase::IgnoreCase) &&
				!Path.Contains(*Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// No parent class filter for level blueprints
		if (ParentClassFilter && !ParentClassFilter->IsEmpty())
		{
			if (!FString(TEXT("LevelScriptActor")).Contains(*ParentClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("parentClass"), TEXT("LevelScriptActor"));
		Entry->SetBoolField(TEXT("isLevelBlueprint"), true);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetNumberField(TEXT("total"), AllBlueprintAssets.Num() + AllMapAssets.Num());
	Result->SetArrayField(TEXT("blueprints"), Entries);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleGetBlueprint(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' parameter"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	return JsonToString(SerializeBlueprint(BP));
}

FString FBlueprintMCPServer::HandleGetGraph(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	const FString* GraphName = Params.Find(TEXT("graph"));
	if (!Name || Name->IsEmpty() || !GraphName || GraphName->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' or 'graph' parameter"));
	}

	// URL-decode graph name to handle spaces encoded as '+' or '%20'
	FString DecodedGraphName = UrlDecode(*GraphName);

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(DecodedGraphName, ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> GraphJson = SerializeGraph(Graph);
			if (GraphJson.IsValid())
			{
				return JsonToString(GraphJson.ToSharedRef());
			}
		}
	}

	// Not found — list available graphs
	TArray<TSharedPtr<FJsonValue>> GraphNames;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph)
		{
			GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
	}
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph '%s' not found"), *DecodedGraphName));
	E->SetArrayField(TEXT("availableGraphs"), GraphNames);
	return JsonToString(E);
}

FString FBlueprintMCPServer::HandleSearch(const TMap<FString, FString>& Params)
{
	const FString* Query = Params.Find(TEXT("query"));
	if (!Query || Query->IsEmpty())
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), TEXT("Missing 'query' parameter"));
		return JsonToString(E);
	}

	const FString* PathFilter = Params.Find(TEXT("path"));

	int32 MaxResults = 50;
	if (const FString* M = Params.Find(TEXT("maxResults")))
	{
		MaxResults = FMath::Clamp(FCString::Atoi(**M), 1, 200);
	}

	// Build a combined list of all searchable blueprints (regular + level)
	auto SearchBlueprint = [&](const FString& AssetName, const FString& Path, UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutResults)
	{
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);

		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || OutResults.Num() >= MaxResults) break;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || OutResults.Num() >= MaxResults) break;

				FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

				FString FuncName, EventName, VarName;
				if (auto* CF = Cast<UK2Node_CallFunction>(Node))
				{
					FuncName = CF->FunctionReference.GetMemberName().ToString();
				}
				else if (auto* Ev = Cast<UK2Node_Event>(Node))
				{
					EventName = Ev->EventReference.GetMemberName().ToString();
				}
				else if (auto* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					EventName = CE->CustomFunctionName.ToString();
				}
				else if (auto* VG = Cast<UK2Node_VariableGet>(Node))
				{
					VarName = VG->GetVarName().ToString();
				}
				else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
				{
					VarName = VS->GetVarName().ToString();
				}

				bool bMatch = Title.Contains(*Query, ESearchCase::IgnoreCase) ||
					(!FuncName.IsEmpty() && FuncName.Contains(*Query, ESearchCase::IgnoreCase)) ||
					(!EventName.IsEmpty() && EventName.Contains(*Query, ESearchCase::IgnoreCase)) ||
					(!VarName.IsEmpty() && VarName.Contains(*Query, ESearchCase::IgnoreCase));

				if (bMatch)
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("blueprint"), AssetName);
					R->SetStringField(TEXT("blueprintPath"), Path);
					R->SetStringField(TEXT("graph"), Graph->GetName());
					R->SetStringField(TEXT("nodeTitle"), Title);
					R->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
					if (!FuncName.IsEmpty()) R->SetStringField(TEXT("functionName"), FuncName);
					if (!EventName.IsEmpty()) R->SetStringField(TEXT("eventName"), EventName);
					if (!VarName.IsEmpty()) R->SetStringField(TEXT("variableName"), VarName);
					OutResults.Add(MakeShared<FJsonValueObject>(R));
				}
			}
		}
	};

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = Asset.PackageName.ToString();
		if (PathFilter && !PathFilter->IsEmpty() && !Path.Contains(*PathFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
		if (!BP) continue;

		SearchBlueprint(Asset.AssetName.ToString(), Path, BP, Results);
	}

	// Also search level blueprints
	for (FAssetData& MapAsset : AllMapAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = MapAsset.PackageName.ToString();
		if (PathFilter && !PathFilter->IsEmpty() && !Path.Contains(*PathFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UWorld* World = Cast<UWorld>(MapAsset.GetAsset());
		if (!World || !World->PersistentLevel) continue;
		ULevelScriptBlueprint* LevelBP = World->PersistentLevel->GetLevelScriptBlueprint(false);
		if (!LevelBP) continue;

		SearchBlueprint(MapAsset.AssetName.ToString(), Path, LevelBP, Results);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), *Query);
	Result->SetNumberField(TEXT("resultCount"), Results.Num());
	Result->SetArrayField(TEXT("results"), Results);
	return JsonToString(Result);
}

// ============================================================
// HandleTestSave — load a Blueprint and save it unmodified (diagnostic)
// ============================================================

FString FBlueprintMCPServer::HandleTestSave(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' query parameter"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: test-save requested for '%s'"), **Name);

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: test-save — loaded '%s', GeneratedClass=%s"),
		*BP->GetName(),
		BP->GeneratedClass ? *BP->GeneratedClass->GetName() : TEXT("null"));

	// Attempt save with NO modifications
	bool bSaved = SaveBlueprintPackage(BP);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), *Name);
	Result->SetStringField(TEXT("packagePath"), BP->GetPackage()->GetName());
	Result->SetBoolField(TEXT("hasGeneratedClass"), BP->GeneratedClass != nullptr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleFindReferences — find all Blueprints referencing an asset
// ============================================================

FString FBlueprintMCPServer::HandleFindReferences(const TMap<FString, FString>& Params)
{
	const FString* AssetPath = Params.Find(TEXT("assetPath"));
	if (!AssetPath || AssetPath->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'assetPath' query parameter"));
	}

	IAssetRegistry& Registry = *IAssetRegistry::Get();

	TArray<FName> Referencers;
	Registry.GetReferencers(FName(**AssetPath), Referencers);

	// Build set of known Blueprint package names for filtering
	TSet<FString> BlueprintPackages;
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		BlueprintPackages.Add(Asset.PackageName.ToString());
	}

	TArray<TSharedPtr<FJsonValue>> BPRefs;
	TArray<TSharedPtr<FJsonValue>> OtherRefs;
	for (const FName& Ref : Referencers)
	{
		FString RefStr = Ref.ToString();
		if (BlueprintPackages.Contains(RefStr))
		{
			BPRefs.Add(MakeShared<FJsonValueString>(RefStr));
		}
		else
		{
			OtherRefs.Add(MakeShared<FJsonValueString>(RefStr));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), **AssetPath);
	Result->SetNumberField(TEXT("totalReferencers"), Referencers.Num());
	Result->SetNumberField(TEXT("blueprintReferencerCount"), BPRefs.Num());
	Result->SetArrayField(TEXT("blueprintReferencers"), BPRefs);
	Result->SetNumberField(TEXT("otherReferencerCount"), OtherRefs.Num());
	Result->SetArrayField(TEXT("otherReferencers"), OtherRefs);
	return JsonToString(Result);
}

// ============================================================
// HandleReplaceFunctionCalls — redirect function call nodes
// ============================================================

FString FBlueprintMCPServer::HandleReplaceFunctionCalls(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString OldClassName = Json->GetStringField(TEXT("oldClass"));
	FString NewClassName = Json->GetStringField(TEXT("newClass"));

	if (BlueprintName.IsEmpty() || OldClassName.IsEmpty() || NewClassName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, oldClass, newClass"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the new class — try several search strategies
	UClass* NewClass = nullptr;

	// Try finding the class across all loaded modules
	NewClass = FindFirstObject<UClass>(*NewClassName);

	// Try with U prefix stripped/added
	if (!NewClass && NewClassName.StartsWith(TEXT("U")))
	{
		FString WithoutU = NewClassName.Mid(1);
		NewClass = FindFirstObject<UClass>(*WithoutU);
	}
	if (!NewClass && !NewClassName.StartsWith(TEXT("U")))
	{
		NewClass = FindFirstObject<UClass>(*FString::Printf(TEXT("U%s"), *NewClassName));
	}

	// Broader search across all modules
	if (!NewClass)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == NewClassName || It->GetName() == FString::Printf(TEXT("U%s"), *NewClassName))
			{
				NewClass = *It;
				break;
			}
		}
	}

	if (!NewClass)
	{
		return MakeErrorJson(FString::Printf(TEXT("Could not find class '%s'"), *NewClassName));
	}

	// Check for dry run
	bool bDryRun = false;
	if (Json->HasField(TEXT("dryRun")))
	{
		bDryRun = Json->GetBoolField(TEXT("dryRun"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %s function calls in '%s': %s -> %s (%s)"),
		bDryRun ? TEXT("[DRY RUN] Analyzing replacement of") : TEXT("Replacing"),
		*BlueprintName, *OldClassName, *NewClassName, *NewClass->GetPathName());

	// Find all CallFunction nodes
	TArray<UK2Node_CallFunction*> AllCallNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CallFunction>(BP, AllCallNodes);

	int32 ReplacedCount = 0;
	TArray<TSharedPtr<FJsonValue>> BrokenConnections;

	for (UK2Node_CallFunction* CallNode : AllCallNodes)
	{
		UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass();
		if (!ParentClass)
		{
			continue;
		}

		// Match by class name (with or without U prefix, and _C suffix for BP classes)
		FString ParentName = ParentClass->GetName();
		bool bMatch = ParentName == OldClassName ||
			ParentName == FString::Printf(TEXT("%s_C"), *OldClassName) ||
			ParentName == FString::Printf(TEXT("U%s"), *OldClassName) ||
			(OldClassName.StartsWith(TEXT("U")) && ParentName == OldClassName.Mid(1)) ||
			(OldClassName.EndsWith(TEXT("_C")) && ParentName == OldClassName.LeftChop(2));

		if (!bMatch)
		{
			continue;
		}

		FName FuncName = CallNode->FunctionReference.GetMemberName();

		// Find the matching function in the new class
		UFunction* NewFunc = NewClass->FindFunctionByName(FuncName);
		if (!NewFunc)
		{
			UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Function '%s' not found in '%s', skipping node"),
				*FuncName.ToString(), *NewClassName);

			TSharedRef<FJsonObject> Warning = MakeShared<FJsonObject>();
			Warning->SetStringField(TEXT("type"), TEXT("functionNotFound"));
			Warning->SetStringField(TEXT("functionName"), FuncName.ToString());
			Warning->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
			BrokenConnections.Add(MakeShared<FJsonValueObject>(Warning));
			continue;
		}

		if (bDryRun)
		{
			// In dry run mode: report what would be affected without modifying
			ReplacedCount++;

			// Check which pins have connections that might break
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (!Pin || Pin->LinkedTo.Num() == 0) continue;

				// Check if the new function has a matching parameter
				bool bPinExistsInNew = false;
				for (TFieldIterator<FProperty> PropIt(NewFunc); PropIt; ++PropIt)
				{
					if (PropIt->GetFName() == Pin->PinName ||
						Pin->PinName == UEdGraphSchema_K2::PN_Execute ||
						Pin->PinName == UEdGraphSchema_K2::PN_Then ||
						Pin->PinName == UEdGraphSchema_K2::PN_Self ||
						Pin->PinName == UEdGraphSchema_K2::PN_ReturnValue)
					{
						bPinExistsInNew = true;
						break;
					}
				}

				if (!bPinExistsInNew)
				{
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNode())
						{
							TSharedRef<FJsonObject> AtRisk = MakeShared<FJsonObject>();
							AtRisk->SetStringField(TEXT("type"), TEXT("connectionAtRisk"));
							AtRisk->SetStringField(TEXT("functionName"), FuncName.ToString());
							AtRisk->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
							AtRisk->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
							AtRisk->SetStringField(TEXT("connectedToNode"), Linked->GetOwningNode()->NodeGuid.ToString());
							AtRisk->SetStringField(TEXT("connectedToPin"), Linked->PinName.ToString());
							BrokenConnections.Add(MakeShared<FJsonValueObject>(AtRisk));
						}
					}
				}
			}
		}
		else
		{
			// Record existing pin connections before replacement
			TMap<FString, TArray<TPair<FString, FString>>> OldPinConnections;
			for (UEdGraphPin* Pin : CallNode->Pins)
			{
				if (Pin->LinkedTo.Num() > 0)
				{
					TArray<TPair<FString, FString>> Links;
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNode())
						{
							Links.Add(TPair<FString, FString>(
								Linked->GetOwningNode()->NodeGuid.ToString(),
								Linked->PinName.ToString()));
						}
					}
					OldPinConnections.Add(Pin->PinName.ToString(), Links);
				}
			}

			// Replace the function reference
			CallNode->SetFromFunction(NewFunc);
			ReplacedCount++;

			// Check which connections survived
			for (auto& Pair : OldPinConnections)
			{
				const FString& PinName = Pair.Key;
				const TArray<TPair<FString, FString>>& OldLinks = Pair.Value;

				UEdGraphPin* NewPin = CallNode->FindPin(FName(*PinName));
				for (auto& Link : OldLinks)
				{
					bool bStillConnected = false;
					if (NewPin)
					{
						for (UEdGraphPin* L : NewPin->LinkedTo)
						{
							if (L && L->GetOwningNode() &&
								L->GetOwningNode()->NodeGuid.ToString() == Link.Key &&
								L->PinName.ToString() == Link.Value)
							{
								bStillConnected = true;
								break;
							}
						}
					}
					if (!bStillConnected)
					{
						TSharedRef<FJsonObject> Broken = MakeShared<FJsonObject>();
						Broken->SetStringField(TEXT("type"), TEXT("connectionLost"));
						Broken->SetStringField(TEXT("functionName"), FuncName.ToString());
						Broken->SetStringField(TEXT("nodeId"), CallNode->NodeGuid.ToString());
						Broken->SetStringField(TEXT("pinName"), PinName);
						Broken->SetStringField(TEXT("wasConnectedToNode"), Link.Key);
						Broken->SetStringField(TEXT("wasConnectedToPin"), Link.Value);
						BrokenConnections.Add(MakeShared<FJsonValueObject>(Broken));
					}
				}
			}
		}
	}

	if (bDryRun)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetNumberField(TEXT("wouldReplaceCount"), ReplacedCount);
		Result->SetNumberField(TEXT("connectionsAtRisk"), BrokenConnections.Num());
		Result->SetArrayField(TEXT("connectionsAtRisk"), BrokenConnections);
		return JsonToString(Result);
	}

	// Save — guard flags and SEH protection are handled inside SaveBlueprintPackage
	if (ReplacedCount > 0)
	{
		bool bSaved = SaveBlueprintPackage(BP);
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Replaced %d function call(s), save %s"),
			ReplacedCount, bSaved ? TEXT("succeeded") : TEXT("failed"));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetNumberField(TEXT("replacedCount"), ReplacedCount);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetNumberField(TEXT("brokenConnectionCount"), BrokenConnections.Num());
		Result->SetArrayField(TEXT("brokenConnections"), BrokenConnections);
		return JsonToString(Result);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("replacedCount"), 0);
	Result->SetStringField(TEXT("message"), FString::Printf(
		TEXT("No function call nodes found targeting class '%s'"), *OldClassName));
	return JsonToString(Result);
}

// ============================================================
// HandleChangeVariableType — change a Blueprint member variable's type
// ============================================================

FString FBlueprintMCPServer::HandleChangeVariableType(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString VariableName = Json->GetStringField(TEXT("variable"));
	FString NewTypeName = Json->GetStringField(TEXT("newType"));
	FString TypeCategory = Json->GetStringField(TEXT("typeCategory"));

	if (BlueprintName.IsEmpty() || VariableName.IsEmpty() || NewTypeName.IsEmpty() || TypeCategory.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, variable, newType, typeCategory"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Verify variable exists
	bool bVarFound = false;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName.ToString() == VariableName)
		{
			bVarFound = true;
			break;
		}
	}
	if (!bVarFound)
	{
		return MakeErrorJson(FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *BlueprintName));
	}

	// Build the new pin type
	FEdGraphPinType NewPinType;

	// Strip F/E/U prefix to get the UE internal name
	FString InternalName = NewTypeName;
	if ((TypeCategory == TEXT("struct") && InternalName.StartsWith(TEXT("F"))) ||
		(TypeCategory == TEXT("enum") && InternalName.StartsWith(TEXT("E"))))
	{
		InternalName = InternalName.Mid(1);
	}

	if (TypeCategory == TEXT("struct"))
	{
		// Find the struct
		UScriptStruct* FoundStruct = nullptr;

		// Try finding the struct across all loaded modules
		FoundStruct = FindFirstObject<UScriptStruct>(*InternalName);

		// Try broader search
		if (!FoundStruct)
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (It->GetName() == InternalName)
				{
					FoundStruct = *It;
					break;
				}
			}
		}

		if (!FoundStruct)
		{
			return MakeErrorJson(FString::Printf(TEXT("Struct '%s' not found"), *NewTypeName));
		}

		NewPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		NewPinType.PinSubCategoryObject = FoundStruct;
	}
	else if (TypeCategory == TEXT("enum"))
	{
		// Find the enum
		UEnum* FoundEnum = nullptr;

		// Try finding the enum across all loaded modules
		FoundEnum = FindFirstObject<UEnum>(*InternalName);

		// Try broader search
		if (!FoundEnum)
		{
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				if (It->GetName() == InternalName)
				{
					FoundEnum = *It;
					break;
				}
			}
		}

		if (!FoundEnum)
		{
			return MakeErrorJson(FString::Printf(TEXT("Enum '%s' not found"), *NewTypeName));
		}

		// Use PC_Byte for BP enums (uint8-backed), PC_Enum for native C++ enum class
		if (FoundEnum->GetCppForm() == UEnum::ECppForm::EnumClass)
		{
			NewPinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
		}
		else
		{
			NewPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		NewPinType.PinSubCategoryObject = FoundEnum;
	}
	else
	{
		return MakeErrorJson(FString::Printf(TEXT("Unsupported typeCategory '%s'. Use 'struct' or 'enum'."), *TypeCategory));
	}

	// Check for dry run
	bool bDryRun = false;
	if (Json->HasField(TEXT("dryRun")))
	{
		bDryRun = Json->GetBoolField(TEXT("dryRun"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %s variable '%s' in '%s' to %s (%s)"),
		bDryRun ? TEXT("[DRY RUN] Analyzing change of") : TEXT("Changing"),
		*VariableName, *BlueprintName, *NewTypeName, *TypeCategory);

	// Analyze affected nodes (get/set nodes for this variable)
	TArray<TSharedPtr<FJsonValue>> AffectedNodes;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (auto* VG = Cast<UK2Node_VariableGet>(Node))
			{
				if (VG->GetVarName().ToString() == VariableName)
				{
					TSharedRef<FJsonObject> AffNode = MakeShared<FJsonObject>();
					AffNode->SetStringField(TEXT("nodeId"), VG->NodeGuid.ToString());
					AffNode->SetStringField(TEXT("nodeType"), TEXT("VariableGet"));
					AffNode->SetStringField(TEXT("graph"), Graph->GetName());
					// Check which pins would be affected
					TArray<TSharedPtr<FJsonValue>> AffPins;
					for (UEdGraphPin* Pin : VG->Pins)
					{
						if (Pin && Pin->LinkedTo.Num() > 0 && Pin->Direction == EGPD_Output)
						{
							AffPins.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s (connected to %d pin(s))"),
									*Pin->PinName.ToString(), Pin->LinkedTo.Num())));
						}
					}
					AffNode->SetArrayField(TEXT("affectedPins"), AffPins);
					AffectedNodes.Add(MakeShared<FJsonValueObject>(AffNode));
				}
			}
			else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
			{
				if (VS->GetVarName().ToString() == VariableName)
				{
					TSharedRef<FJsonObject> AffNode = MakeShared<FJsonObject>();
					AffNode->SetStringField(TEXT("nodeId"), VS->NodeGuid.ToString());
					AffNode->SetStringField(TEXT("nodeType"), TEXT("VariableSet"));
					AffNode->SetStringField(TEXT("graph"), Graph->GetName());
					TArray<TSharedPtr<FJsonValue>> AffPins;
					for (UEdGraphPin* Pin : VS->Pins)
					{
						if (Pin && Pin->LinkedTo.Num() > 0)
						{
							AffPins.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s (connected to %d pin(s))"),
									*Pin->PinName.ToString(), Pin->LinkedTo.Num())));
						}
					}
					AffNode->SetArrayField(TEXT("affectedPins"), AffPins);
					AffectedNodes.Add(MakeShared<FJsonValueObject>(AffNode));
				}
			}
		}
	}

	if (bDryRun)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variable"), VariableName);
		Result->SetStringField(TEXT("newType"), NewTypeName);
		Result->SetStringField(TEXT("typeCategory"), TypeCategory);
		Result->SetNumberField(TEXT("affectedNodeCount"), AffectedNodes.Num());
		Result->SetArrayField(TEXT("affectedNodes"), AffectedNodes);
		return JsonToString(Result);
	}

	// Directly modify the variable type in the description array.
	for (FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == FName(*VariableName))
		{
			Var.VarType = NewPinType;
			break;
		}
	}

	// Save
	bool bSaved = SaveBlueprintPackage(BP);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Variable type changed, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	// Return updated variable state
	TSharedRef<FJsonObject> UpdatedVar = MakeShared<FJsonObject>();
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == FName(*VariableName))
		{
			UpdatedVar->SetStringField(TEXT("name"), Var.VarName.ToString());
			UpdatedVar->SetStringField(TEXT("type"), Var.VarType.PinCategory.ToString());
			if (Var.VarType.PinSubCategoryObject.IsValid())
				UpdatedVar->SetStringField(TEXT("subtype"), Var.VarType.PinSubCategoryObject->GetName());
			UpdatedVar->SetBoolField(TEXT("isArray"), Var.VarType.IsArray());
			break;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("variable"), VariableName);
	Result->SetStringField(TEXT("newType"), NewTypeName);
	Result->SetStringField(TEXT("typeCategory"), TypeCategory);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetObjectField(TEXT("updatedVariable"), UpdatedVar);
	Result->SetArrayField(TEXT("affectedNodes"), AffectedNodes);
	return JsonToString(Result);
}

// ============================================================
// HandleChangeFunctionParamType
// ============================================================

FString FBlueprintMCPServer::HandleChangeFunctionParamType(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString FunctionName = Json->GetStringField(TEXT("functionName"));
	FString ParamName = Json->GetStringField(TEXT("paramName"));
	FString NewTypeName = Json->GetStringField(TEXT("newType"));

	if (BlueprintName.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() || NewTypeName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, functionName, paramName, newType"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the new struct type — strip F prefix for UE internal name
	FString InternalName = NewTypeName;
	if (InternalName.StartsWith(TEXT("F")))
	{
		InternalName = InternalName.Mid(1);
	}

	UScriptStruct* FoundStruct = nullptr;

	// Try finding the struct across all loaded modules
	FoundStruct = FindFirstObject<UScriptStruct>(*InternalName);

	// Broader search
	if (!FoundStruct)
	{
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			if (It->GetName() == InternalName)
			{
				FoundStruct = *It;
				break;
			}
		}
	}

	if (!FoundStruct)
	{
		return MakeErrorJson(FString::Printf(TEXT("Struct '%s' not found"), *NewTypeName));
	}

	FEdGraphPinType NewPinType;
	NewPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	NewPinType.PinSubCategoryObject = FoundStruct;

	// Find the entry node: K2Node_FunctionEntry in a function graph,
	// or K2Node_CustomEvent in any graph
	UK2Node_EditablePinBase* EntryNode = nullptr;
	FString FoundNodeType;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	// Strategy 1: Look for a function graph matching the name
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					EntryNode = FuncEntry;
					FoundNodeType = TEXT("FunctionEntry");
					break;
				}
			}
			if (EntryNode) break;
		}
	}

	// Strategy 2: Search for a K2Node_CustomEvent with matching CustomFunctionName
	if (!EntryNode)
	{
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CustomEvent->CustomFunctionName.ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
					{
						EntryNode = CustomEvent;
						FoundNodeType = TEXT("CustomEvent");
						break;
					}
				}
			}
			if (EntryNode) break;
		}
	}

	if (!EntryNode)
	{
		// List available functions/events for debugging
		TArray<TSharedPtr<FJsonValue>> Available;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
				{
					Available.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("function:%s"), *Graph->GetName())));
				}
				else if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					Available.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("event:%s"), *CE->CustomFunctionName.ToString())));
				}
			}
		}

		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Function or custom event '%s' not found in Blueprint '%s'"),
			*FunctionName, *BlueprintName));
		E->SetArrayField(TEXT("availableFunctionsAndEvents"), Available);
		return JsonToString(E);
	}

	// Find the UserDefinedPin matching paramName
	bool bPinFound = false;
	for (TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
	{
		if (PinInfo.IsValid() && PinInfo->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			PinInfo->PinType = NewPinType;
			bPinFound = true;
			break;
		}
	}

	if (!bPinFound)
	{
		// List available params for debugging
		TArray<TSharedPtr<FJsonValue>> ParamNames;
		for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
		{
			if (PinInfo.IsValid())
			{
				ParamNames.Add(MakeShared<FJsonValueString>(PinInfo->PinName.ToString()));
			}
		}

		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Parameter '%s' not found in %s '%s'"),
			*ParamName, *FoundNodeType, *FunctionName));
		E->SetArrayField(TEXT("availableParams"), ParamNames);
		return JsonToString(E);
	}

	// Check for dry run
	bool bDryRun = false;
	if (Json->HasField(TEXT("dryRun")))
	{
		bDryRun = Json->GetBoolField(TEXT("dryRun"));
	}

	if (bDryRun)
	{
		// Analyze what would change: report connected pins that may disconnect
		TArray<TSharedPtr<FJsonValue>> AffectedPins;
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase) && Pin->LinkedTo.Num() > 0)
			{
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						TSharedRef<FJsonObject> AffPin = MakeShared<FJsonObject>();
						AffPin->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
						AffPin->SetStringField(TEXT("connectedToNode"), Linked->GetOwningNode()->NodeGuid.ToString());
						AffPin->SetStringField(TEXT("connectedToPin"), Linked->PinName.ToString());
						AffPin->SetStringField(TEXT("currentType"), Pin->PinType.PinCategory.ToString());
						if (Pin->PinType.PinSubCategoryObject.IsValid())
							AffPin->SetStringField(TEXT("currentSubtype"), Pin->PinType.PinSubCategoryObject->GetName());
						AffectedPins.Add(MakeShared<FJsonValueObject>(AffPin));
					}
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("functionName"), FunctionName);
		Result->SetStringField(TEXT("paramName"), ParamName);
		Result->SetStringField(TEXT("newType"), NewTypeName);
		Result->SetStringField(TEXT("nodeType"), FoundNodeType);
		Result->SetStringField(TEXT("nodeId"), EntryNode->NodeGuid.ToString());
		Result->SetNumberField(TEXT("connectionsAtRisk"), AffectedPins.Num());
		Result->SetArrayField(TEXT("affectedPins"), AffectedPins);
		return JsonToString(Result);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Changing param '%s' in %s '%s' of '%s' to %s"),
		*ParamName, *FoundNodeType, *FunctionName, *BlueprintName, *NewTypeName);

	// Reconstruct the node to update output pins with the new type
	EntryNode->ReconstructNode();

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Parameter type changed, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	// Serialize the updated entry node state
	TSharedPtr<FJsonObject> UpdatedNodeState = SerializeNode(EntryNode);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("functionName"), FunctionName);
	Result->SetStringField(TEXT("paramName"), ParamName);
	Result->SetStringField(TEXT("newType"), NewTypeName);
	Result->SetStringField(TEXT("nodeType"), FoundNodeType);
	Result->SetStringField(TEXT("nodeId"), EntryNode->NodeGuid.ToString());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (UpdatedNodeState.IsValid())
	{
		Result->SetObjectField(TEXT("updatedNode"), UpdatedNodeState);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveFunctionParameter
// ============================================================

FString FBlueprintMCPServer::HandleRemoveFunctionParameter(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString FunctionName = Json->GetStringField(TEXT("functionName"));
	FString ParamName = Json->GetStringField(TEXT("paramName"));

	if (BlueprintName.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, functionName, paramName"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the entry node
	UK2Node_EditablePinBase* EntryNode = nullptr;
	FString FoundNodeType;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	// Strategy 1: Look for a function graph matching the name
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					EntryNode = FuncEntry;
					FoundNodeType = TEXT("FunctionEntry");
					break;
				}
			}
			if (EntryNode) break;
		}
	}

	// Strategy 2: Search for a K2Node_CustomEvent with matching CustomFunctionName
	if (!EntryNode)
	{
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CustomEvent->CustomFunctionName.ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
					{
						EntryNode = CustomEvent;
						FoundNodeType = TEXT("CustomEvent");
						break;
					}
				}
			}
			if (EntryNode) break;
		}
	}

	if (!EntryNode)
	{
		// List available functions/events for debugging
		TArray<TSharedPtr<FJsonValue>> Available;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
				{
					Available.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("function:%s"), *Graph->GetName())));
				}
				else if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					Available.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("event:%s"), *CE->CustomFunctionName.ToString())));
				}
			}
		}

		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Function or custom event '%s' not found in Blueprint '%s'"),
			*FunctionName, *BlueprintName));
		E->SetArrayField(TEXT("availableFunctionsAndEvents"), Available);
		return JsonToString(E);
	}

	// Find and remove the UserDefinedPin matching paramName
	int32 RemovedIndex = INDEX_NONE;
	for (int32 i = 0; i < EntryNode->UserDefinedPins.Num(); ++i)
	{
		if (EntryNode->UserDefinedPins[i].IsValid() &&
			EntryNode->UserDefinedPins[i]->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			RemovedIndex = i;
			break;
		}
	}

	if (RemovedIndex == INDEX_NONE)
	{
		// List available params for debugging
		TArray<TSharedPtr<FJsonValue>> ParamNames;
		for (const TSharedPtr<FUserPinInfo>& PinInfo : EntryNode->UserDefinedPins)
		{
			if (PinInfo.IsValid())
			{
				ParamNames.Add(MakeShared<FJsonValueString>(PinInfo->PinName.ToString()));
			}
		}

		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Parameter '%s' not found in %s '%s'"),
			*ParamName, *FoundNodeType, *FunctionName));
		E->SetArrayField(TEXT("availableParams"), ParamNames);
		return JsonToString(E);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removing param '%s' from %s '%s' in '%s'"),
		*ParamName, *FoundNodeType, *FunctionName, *BlueprintName);

	// Remove the pin
	EntryNode->UserDefinedPins.RemoveAt(RemovedIndex);

	// Reconstruct the node to update output pins
	EntryNode->ReconstructNode();

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Parameter removed, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("functionName"), FunctionName);
	Result->SetStringField(TEXT("paramName"), ParamName);
	Result->SetStringField(TEXT("nodeType"), FoundNodeType);
	Result->SetStringField(TEXT("nodeId"), EntryNode->NodeGuid.ToString());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleDeleteAsset — delete a .uasset after verifying no references
// ============================================================

FString FBlueprintMCPServer::HandleDeleteAsset(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: assetPath"));
	}

	bool bForce = false;
	if (Json->HasField(TEXT("force")))
	{
		bForce = Json->GetBoolField(TEXT("force"));
	}

	// Check if asset file exists on disk
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		AssetPath, FPackageName::GetAssetPackageExtension());
	PackageFilename = FPaths::ConvertRelativePathToFull(PackageFilename);

	if (!IFileManager::Get().FileExists(*PackageFilename))
	{
		return MakeErrorJson(FString::Printf(TEXT("Asset file not found on disk: %s"), *PackageFilename));
	}

	// Check references
	IAssetRegistry& Registry = *IAssetRegistry::Get();
	TArray<FName> Referencers;
	Registry.GetReferencers(FName(*AssetPath), Referencers);

	// Filter out self-references
	Referencers.RemoveAll([&AssetPath](const FName& Ref) {
		return Ref.ToString() == AssetPath;
	});

	if (Referencers.Num() > 0 && !bForce)
	{
		// Classify references as "live" (loaded in memory) vs "stale" (only on disk)
		TArray<TSharedPtr<FJsonValue>> LiveRefs;
		TArray<TSharedPtr<FJsonValue>> StaleRefs;
		for (const FName& Ref : Referencers)
		{
			FString RefStr = Ref.ToString();
			UPackage* RefPackage = FindPackage(nullptr, *RefStr);
			if (RefPackage)
			{
				LiveRefs.Add(MakeShared<FJsonValueString>(RefStr));
			}
			else
			{
				StaleRefs.Add(MakeShared<FJsonValueString>(RefStr));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("error"), TEXT("Asset is still referenced. Remove all references first."));
		Result->SetStringField(TEXT("assetPath"), AssetPath);
		Result->SetNumberField(TEXT("referencerCount"), Referencers.Num());
		Result->SetNumberField(TEXT("liveReferencerCount"), LiveRefs.Num());
		Result->SetArrayField(TEXT("liveReferencers"), LiveRefs);
		Result->SetNumberField(TEXT("staleReferencerCount"), StaleRefs.Num());
		Result->SetArrayField(TEXT("staleReferencers"), StaleRefs);
		Result->SetStringField(TEXT("suggestion"),
			StaleRefs.Num() > 0
				? TEXT("Some references may be stale. Consider force=true to skip the reference check, or use change_variable_type to migrate references first.")
				: TEXT("All references are live. Migrate with change_variable_type or replace_function_calls before deleting."));
		return JsonToString(Result);
	}

	// Force delete: unload the package from memory first
	TArray<TSharedPtr<FJsonValue>> RefWarnings;
	if (bForce)
	{
		// Collect reference warnings when force-deleting with existing references
		for (const FName& Ref : Referencers)
		{
			RefWarnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Warning: '%s' still references this asset"), *Ref.ToString())));
		}

		UPackage* Package = FindPackage(nullptr, *AssetPath);
		if (Package)
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Force-unloading package '%s' from memory"), *AssetPath);

			// Collect all objects in this package
			TArray<UObject*> ObjectsInPackage;
			GetObjectsWithPackage(Package, ObjectsInPackage);

			// Clear flags and remove from root to allow GC
			for (UObject* Obj : ObjectsInPackage)
			{
				if (Obj)
				{
					Obj->ClearFlags(RF_Standalone | RF_Public);
					Obj->RemoveFromRoot();
				}
			}
			Package->ClearFlags(RF_Standalone | RF_Public);
			Package->RemoveFromRoot();

			// Reset loaders to release file handles
			ResetLoaders(Package);

			// Force garbage collection to free the objects
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deleting asset '%s' (%s)%s"),
		*AssetPath, *PackageFilename, bForce ? TEXT(" [FORCE]") : TEXT(""));

	// Delete the file on disk
	bool bDeleted = IFileManager::Get().Delete(*PackageFilename, false, true);

	if (bDeleted)
	{
		// Remove from our cached asset list if present
		AllBlueprintAssets.RemoveAll([&AssetPath](const FAssetData& Asset) {
			return Asset.PackageName.ToString() == AssetPath;
		});

		// Trigger an asset registry rescan so it notices the deletion
		TArray<FString> PathsToScan;
		int32 LastSlash;
		if (AssetPath.FindLastChar(TEXT('/'), LastSlash))
		{
			PathsToScan.Add(AssetPath.Left(LastSlash));
		}
		if (PathsToScan.Num() > 0)
		{
			Registry.ScanPathsSynchronous(PathsToScan, true);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bDeleted);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("filename"), PackageFilename);
	Result->SetBoolField(TEXT("forced"), bForce);
	if (!bDeleted)
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to delete file from disk"));
	}
	if (RefWarnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), RefWarnings);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleConnectPins — wire two pins together
// ============================================================

FString FBlueprintMCPServer::HandleConnectPins(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString SourceNodeId = Json->GetStringField(TEXT("sourceNodeId"));
	FString SourcePinName = Json->GetStringField(TEXT("sourcePinName"));
	FString TargetNodeId = Json->GetStringField(TEXT("targetNodeId"));
	FString TargetPinName = Json->GetStringField(TEXT("targetPinName"));

	if (BlueprintName.IsEmpty() || SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() ||
		TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, sourceNodeId, sourcePinName, targetNodeId, targetPinName"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find source node
	UEdGraph* SourceGraph = nullptr;
	UEdGraphNode* SourceNode = FindNodeByGuid(BP, SourceNodeId, &SourceGraph);
	if (!SourceNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId));
	}

	// Find target node
	UEdGraphNode* TargetNode = FindNodeByGuid(BP, TargetNodeId);
	if (!TargetNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));
	}

	// Find source pin
	UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
	if (!SourcePin)
	{
		// List available pins for debugging
		TArray<TSharedPtr<FJsonValue>> PinNames;
		for (UEdGraphPin* P : SourceNode->Pins)
		{
			if (P) PinNames.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s (%s)"), *P->PinName.ToString(),
					P->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"))));
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Source pin '%s' not found on node '%s'"),
			*SourcePinName, *SourceNodeId));
		E->SetArrayField(TEXT("availablePins"), PinNames);
		return JsonToString(E);
	}

	// Find target pin
	UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
	if (!TargetPin)
	{
		// List available pins for debugging
		TArray<TSharedPtr<FJsonValue>> PinNames;
		for (UEdGraphPin* P : TargetNode->Pins)
		{
			if (P) PinNames.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s (%s)"), *P->PinName.ToString(),
					P->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"))));
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Target pin '%s' not found on node '%s'"),
			*TargetPinName, *TargetNodeId));
		E->SetArrayField(TEXT("availablePins"), PinNames);
		return JsonToString(E);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Connecting %s.%s -> %s.%s"),
		*SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *SourcePinName,
		*TargetNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *TargetPinName);

	// Try type-validated connection via the schema
	const UEdGraphSchema* Schema = SourceGraph->GetSchema();
	if (!Schema)
	{
		return MakeErrorJson(TEXT("Graph schema not found"));
	}
	bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bConnected);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("sourcePinType"), SourcePin->PinType.PinCategory.ToString());
	if (SourcePin->PinType.PinSubCategoryObject.IsValid())
		Result->SetStringField(TEXT("sourcePinSubtype"), SourcePin->PinType.PinSubCategoryObject->GetName());
	Result->SetStringField(TEXT("targetPinType"), TargetPin->PinType.PinCategory.ToString());
	if (TargetPin->PinType.PinSubCategoryObject.IsValid())
		Result->SetStringField(TEXT("targetPinSubtype"), TargetPin->PinType.PinSubCategoryObject->GetName());

	if (!bConnected)
	{
		// Provide type mismatch details
		FString Reason = FString::Printf(TEXT("Cannot connect %s (%s) to %s (%s) — types are incompatible"),
			*SourcePinName, *SourcePin->PinType.PinCategory.ToString(),
			*TargetPinName, *TargetPin->PinType.PinCategory.ToString());
		Result->SetStringField(TEXT("error"), Reason);
		return JsonToString(Result);
	}

	// Save
	bool bSaved = SaveBlueprintPackage(BP);
	Result->SetBoolField(TEXT("saved"), bSaved);

	// Return updated node state for both source and target
	TSharedPtr<FJsonObject> SourceNodeState = SerializeNode(SourceNode);
	TSharedPtr<FJsonObject> TargetNodeState = SerializeNode(TargetNode);
	if (SourceNodeState.IsValid())
		Result->SetObjectField(TEXT("updatedSourceNode"), SourceNodeState);
	if (TargetNodeState.IsValid())
		Result->SetObjectField(TEXT("updatedTargetNode"), TargetNodeState);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Connection %s, save %s"),
		bConnected ? TEXT("succeeded") : TEXT("failed"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	return JsonToString(Result);
}

// ============================================================
// HandleDisconnectPin — break connections on a pin
// ============================================================

FString FBlueprintMCPServer::HandleDisconnectPin(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));
	FString PinName = Json->GetStringField(TEXT("pinName"));

	if (BlueprintName.IsEmpty() || NodeId.IsEmpty() || PinName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, nodeId, pinName"));
	}

	// Optional: specific target to disconnect from
	FString TargetNodeId = Json->GetStringField(TEXT("targetNodeId"));
	FString TargetPinName = Json->GetStringField(TEXT("targetPinName"));

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find source node
	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	// Find pin
	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		return MakeErrorJson(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId));
	}

	int32 DisconnectedCount = 0;

	if (!TargetNodeId.IsEmpty() && !TargetPinName.IsEmpty())
	{
		// Disconnect a single specific link
		UEdGraphNode* TargetNode = FindNodeByGuid(BP, TargetNodeId);
		if (!TargetNode)
		{
			return MakeErrorJson(FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));
		}

		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
		if (!TargetPin)
		{
			return MakeErrorJson(FString::Printf(TEXT("Target pin '%s' not found on node '%s'"),
				*TargetPinName, *TargetNodeId));
		}

		if (Pin->LinkedTo.Contains(TargetPin))
		{
			Pin->BreakLinkTo(TargetPin);
			DisconnectedCount = 1;
		}
		else
		{
			return MakeErrorJson(TEXT("The specified pins are not connected to each other"));
		}
	}
	else
	{
		// Disconnect all links on this pin
		DisconnectedCount = Pin->LinkedTo.Num();
		if (DisconnectedCount > 0)
		{
			Pin->BreakAllPinLinks(true);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Disconnected %d link(s) from %s.%s"),
		DisconnectedCount, *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *PinName);

	// Save
	bool bSaved = false;
	if (DisconnectedCount > 0)
	{
		bSaved = SaveBlueprintPackage(BP);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("disconnectedCount"), DisconnectedCount);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRefreshAllNodes — refresh all nodes and recompile
// ============================================================

FString FBlueprintMCPServer::HandleRefreshAllNodes(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	if (BlueprintName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: blueprint"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Count graphs and nodes before refresh
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	int32 GraphCount = AllGraphs.Num();
	int32 NodeCount = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph) NodeCount += Graph->Nodes.Num();
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Refreshing all nodes in '%s' (%d graphs, %d nodes)"),
		*BlueprintName, GraphCount, NodeCount);

	// Refresh all nodes with SEH protection
	bool bRefreshCrashed = false;
#if PLATFORM_WINDOWS
	int32 RefreshResult = TryRefreshAllNodesSEH(BP);
	if (RefreshResult != 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: RefreshAllNodes crashed (SEH), proceeding to save"));
		bRefreshCrashed = true;
	}
#else
	FBlueprintEditorUtils::RefreshAllNodes(BP);
#endif

	// Remove orphaned pins from all nodes
	int32 OrphanedPinsRemoved = 0;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			for (int32 i = Node->Pins.Num() - 1; i >= 0; --i)
			{
				UEdGraphPin* Pin = Node->Pins[i];
				if (Pin && Pin->bOrphanedPin)
				{
					Pin->BreakAllPinLinks();
					Node->Pins.RemoveAt(i);
					OrphanedPinsRemoved++;
				}
			}
		}
	}

	if (OrphanedPinsRemoved > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed %d orphaned pins"), OrphanedPinsRemoved);
		// Mark as modified and recompile after orphan removal
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	}

	// Save (which also compiles)
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: RefreshAllNodes complete, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	// Collect compiler warnings and errors from the blueprint status
	TArray<TSharedPtr<FJsonValue>> WarningsArr;
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;

	if (BP->Status == BS_Error)
	{
		ErrorsArr.Add(MakeShared<FJsonValueString>(TEXT("Blueprint has compiler errors after refresh")));
	}

	// Check each graph for nodes with error/warning status
	AllGraphs.Empty();
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->bHasCompilerMessage)
			{
				FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				FString NodeMsg = FString::Printf(TEXT("[%s] %s: %s"),
					*Graph->GetName(), *NodeTitle, *Node->ErrorMsg);
				if (Node->ErrorType == EMessageSeverity::Error)
				{
					ErrorsArr.Add(MakeShared<FJsonValueString>(NodeMsg));
				}
				else
				{
					WarningsArr.Add(MakeShared<FJsonValueString>(NodeMsg));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), !bRefreshCrashed);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("graphCount"), GraphCount);
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetNumberField(TEXT("orphanedPinsRemoved"), OrphanedPinsRemoved);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetArrayField(TEXT("warnings"), WarningsArr);
	Result->SetArrayField(TEXT("errors"), ErrorsArr);
	if (bRefreshCrashed)
	{
		WarningsArr.Add(MakeShared<FJsonValueString>(TEXT("RefreshAllNodes crashed (SEH caught), save was still attempted")));
	}
	return JsonToString(Result);
}

// ============================================================
// HandleSetPinDefault — set the default value of a pin on a node
// ============================================================

FString FBlueprintMCPServer::HandleSetPinDefault(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));
	FString PinName = Json->GetStringField(TEXT("pinName"));
	FString Value = Json->GetStringField(TEXT("value"));

	if (BlueprintName.IsEmpty() || NodeId.IsEmpty() || PinName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, nodeId, pinName"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find node
	UEdGraph* Graph = nullptr;
	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId, &Graph);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	// Find pin
	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		return MakeErrorJson(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId));
	}

	// Only allow setting defaults on input pins
	if (Pin->Direction != EGPD_Input)
	{
		return MakeErrorJson(FString::Printf(TEXT("Pin '%s' is an output pin — can only set defaults on input pins"), *PinName));
	}

	// Store old value for reporting
	FString OldValue = Pin->DefaultValue;

	// Use the schema to set the default value (handles type validation)
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (Schema)
	{
		Schema->TrySetDefaultValue(*Pin, Value);
	}
	else
	{
		// Fallback: set directly
		Pin->DefaultValue = Value;
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: SetPinDefault on '%s' pin '%s': '%s' -> '%s'"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *PinName, *OldValue, *Value);

	// Mark modified and compile
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("pinName"), PinName);
	Result->SetStringField(TEXT("oldValue"), OldValue);
	Result->SetStringField(TEXT("newValue"), Pin->DefaultValue);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleChangeStructNodeType — change the struct type on a Break/Make node
// ============================================================

FString FBlueprintMCPServer::HandleChangeStructNodeType(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));
	FString NewType = Json->GetStringField(TEXT("newType"));

	if (BlueprintName.IsEmpty() || NodeId.IsEmpty() || NewType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, nodeId, newType"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find node
	UEdGraph* Graph = nullptr;
	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId, &Graph);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	// Determine what kind of struct node this is
	UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node);
	UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node);

	if (!BreakNode && !MakeNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' is not a BreakStruct or MakeStruct node (class: %s)"),
			*NodeId, *Node->GetClass()->GetName()));
	}

	// Find the new struct type
	FString SearchName = NewType;
	if (SearchName.StartsWith(TEXT("F")))
	{
		SearchName = SearchName.Mid(1);
	}

	UScriptStruct* NewStruct = FindFirstObject<UScriptStruct>(*SearchName);
	if (!NewStruct)
	{
		// Try with full name including F prefix
		NewStruct = FindFirstObject<UScriptStruct>(*NewType);
	}
	if (!NewStruct)
	{
		return MakeErrorJson(FString::Printf(TEXT("Struct type '%s' not found"), *NewType));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Changing struct node '%s' to type '%s'"),
		*NodeId, *NewStruct->GetName());

	// Helper: extract property base name from a BreakStruct pin name
	auto ExtractPropertyBaseName = [](const FString& PinName) -> FString
	{
		// Find the last underscore before 32 hex chars (GUID)
		int32 LastUnderscore;
		if (PinName.FindLastChar(TEXT('_'), LastUnderscore) && LastUnderscore > 0)
		{
			FString Suffix = PinName.Mid(LastUnderscore + 1);
			if (Suffix.Len() == 32)
			{
				FString WithoutGuid = PinName.Left(LastUnderscore);
				// Strip _Index
				int32 SecondUnderscore;
				if (WithoutGuid.FindLastChar(TEXT('_'), SecondUnderscore) && SecondUnderscore > 0)
				{
					FString IndexStr = WithoutGuid.Mid(SecondUnderscore + 1);
					if (IndexStr.IsNumeric())
					{
						return WithoutGuid.Left(SecondUnderscore);
					}
				}
			}
		}
		return PinName;
	};

	// Remember existing connections keyed by property base name
	struct FPinConnection
	{
		EEdGraphPinDirection Direction;
		TArray<UEdGraphPin*> LinkedPins;
	};
	TMap<FString, FPinConnection> ConnectionsByBaseName;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->LinkedTo.Num() == 0) continue;
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

		FString BaseName = ExtractPropertyBaseName(Pin->PinName.ToString());
		FPinConnection& Conn = ConnectionsByBaseName.FindOrAdd(BaseName);
		Conn.Direction = Pin->Direction;
		Conn.LinkedPins = Pin->LinkedTo;
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Saved %d pin connections to reconnect"), ConnectionsByBaseName.Num());

	// Change the struct type and reconstruct
	if (BreakNode)
	{
		BreakNode->StructType = NewStruct;
	}
	else if (MakeNode)
	{
		MakeNode->StructType = NewStruct;
	}

	// Break all existing links before reconstruction
	Node->BreakAllNodeLinks();

	// Reconstruct to rebuild pins for the new struct type
	Node->ReconstructNode();

	// Reconnect pins by matching property base names
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		return MakeErrorJson(TEXT("Graph schema not found"));
	}
	int32 Reconnected = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> ReconnectDetails;

	for (auto& Pair : ConnectionsByBaseName)
	{
		const FString& BaseName = Pair.Key;
		const FPinConnection& OldConn = Pair.Value;

		// Find matching new pin
		UEdGraphPin* NewPin = nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != OldConn.Direction) continue;
			FString NewBaseName = ExtractPropertyBaseName(Pin->PinName.ToString());
			if (NewBaseName.Equals(BaseName, ESearchCase::IgnoreCase))
			{
				NewPin = Pin;
				break;
			}
		}

		// Also try matching the struct input/output pin (single struct pin)
		if (!NewPin)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != OldConn.Direction) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
					Pin->PinType.PinSubCategoryObject == NewStruct)
				{
					NewPin = Pin;
					break;
				}
			}
		}

		if (NewPin)
		{
			for (UEdGraphPin* Target : OldConn.LinkedPins)
			{
				bool bOK = Schema->TryCreateConnection(NewPin, Target);
				if (bOK)
				{
					Reconnected++;
				}
				else
				{
					Failed++;
				}

				TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("property"), BaseName);
				Detail->SetBoolField(TEXT("connected"), bOK);
				ReconnectDetails.Add(MakeShared<FJsonValueObject>(Detail));
			}
		}
		else
		{
			Failed += OldConn.LinkedPins.Num();
			TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("property"), BaseName);
			Detail->SetBoolField(TEXT("connected"), false);
			Detail->SetStringField(TEXT("reason"), TEXT("No matching pin found on new struct"));
			ReconnectDetails.Add(MakeShared<FJsonValueObject>(Detail));
		}
	}

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	// Return updated node state
	TSharedPtr<FJsonObject> UpdatedNodeState = SerializeNode(Node);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("newStructType"), NewStruct->GetName());
	Result->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
	Result->SetNumberField(TEXT("reconnected"), Reconnected);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("reconnectDetails"), ReconnectDetails);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (UpdatedNodeState.IsValid())
	{
		Result->SetObjectField(TEXT("updatedNode"), UpdatedNodeState);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleDeleteNode — remove a node from a blueprint graph
// ============================================================

FString FBlueprintMCPServer::HandleDeleteNode(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));

	if (BlueprintName.IsEmpty() || NodeId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, nodeId"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find node
	UEdGraph* Graph = nullptr;
	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId, &Graph);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}
	if (!Graph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Graph not found for node '%s'"), *NodeId));
	}

	FString NodeClass = Node->GetClass()->GetName();
	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	FString GraphName = Graph->GetName();

	// Protect root/entry nodes — deleting these leaves the graph in an invalid
	// state with no root node, causing compiler errors that can't be fixed
	// without recreating the entire function/event.
	if (Cast<UK2Node_FunctionEntry>(Node))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot delete FunctionEntry node '%s' in graph '%s'. ")
			TEXT("This is the root node of the function — removing it would leave an empty, uncompilable graph. ")
			TEXT("To remove the entire function, delete it from the Blueprint editor."),
			*NodeTitle, *GraphName));
	}
	if (Cast<UK2Node_Event>(Node))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot delete event entry node '%s' in graph '%s'. ")
			TEXT("This is the root node of the event handler — removing it would leave an empty, uncompilable graph."),
			*NodeTitle, *GraphName));
	}
	if (Cast<UK2Node_CustomEvent>(Node))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot delete CustomEvent entry node '%s' in graph '%s'. ")
			TEXT("This is the root node of the custom event — removing it would leave an empty, uncompilable graph."),
			*NodeTitle, *GraphName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deleting node '%s' (%s) from graph '%s' in '%s'"),
		*NodeId, *NodeTitle, *GraphName, *BlueprintName);

	// Disconnect all pins
	Node->BreakAllNodeLinks();

	// Remove the node from the graph
	Graph->RemoveNode(Node);

	// Save (which also compiles)
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Node deleted, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("nodeClass"), NodeClass);
	Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
	Result->SetStringField(TEXT("graph"), GraphName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleSearchByType — find all usages of a type across blueprints
// ============================================================

FString FBlueprintMCPServer::HandleSearchByType(const TMap<FString, FString>& Params)
{
	const FString* TypeNamePtr = Params.Find(TEXT("typeName"));
	if (!TypeNamePtr || TypeNamePtr->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'typeName' query parameter"));
	}

	FString TypeName = UrlDecode(*TypeNamePtr);
	const FString* Filter = Params.Find(TEXT("filter"));
	FString FilterStr = Filter ? UrlDecode(*Filter) : FString();

	int32 MaxResults = 200;
	if (const FString* M = Params.Find(TEXT("maxResults")))
	{
		MaxResults = FMath::Clamp(FCString::Atoi(**M), 1, 500);
	}

	// Strip F/E/U prefix for comparison
	FString TypeNameNoPrefix = TypeName;
	if (TypeNameNoPrefix.StartsWith(TEXT("F")) || TypeNameNoPrefix.StartsWith(TEXT("E")) || TypeNameNoPrefix.StartsWith(TEXT("U")))
	{
		TypeNameNoPrefix = TypeNameNoPrefix.Mid(1);
	}

	auto MatchesType = [&TypeName, &TypeNameNoPrefix](const FString& TestType) -> bool
	{
		return TestType.Equals(TypeName, ESearchCase::IgnoreCase) ||
			TestType.Equals(TypeNameNoPrefix, ESearchCase::IgnoreCase);
	};

	TArray<TSharedPtr<FJsonValue>> Results;

	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = Asset.PackageName.ToString();
		FString BPName = Asset.AssetName.ToString();

		if (!FilterStr.IsEmpty() && !BPName.Contains(FilterStr, ESearchCase::IgnoreCase) &&
			!Path.Contains(FilterStr, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
		if (!BP) continue;

		// Check variables
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Results.Num() >= MaxResults) break;

			FString VarSubtype;
			if (Var.VarType.PinSubCategoryObject.IsValid())
			{
				VarSubtype = Var.VarType.PinSubCategoryObject->GetName();
			}

			if (MatchesType(VarSubtype) || MatchesType(Var.VarType.PinCategory.ToString()))
			{
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("blueprint"), BPName);
				R->SetStringField(TEXT("blueprintPath"), Path);
				R->SetStringField(TEXT("usage"), TEXT("variable"));
				R->SetStringField(TEXT("location"), Var.VarName.ToString());
				R->SetStringField(TEXT("currentType"), Var.VarType.PinCategory.ToString());
				if (!VarSubtype.IsEmpty())
					R->SetStringField(TEXT("currentSubtype"), VarSubtype);
				Results.Add(MakeShared<FJsonValueObject>(R));
			}
		}

		// Check graphs for function/event params, struct nodes, and pin connections
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || Results.Num() >= MaxResults) break;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || Results.Num() >= MaxResults) break;

				// Check FunctionEntry/CustomEvent parameters
				if (auto* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					for (const TSharedPtr<FUserPinInfo>& PinInfo : FuncEntry->UserDefinedPins)
					{
						if (!PinInfo.IsValid()) continue;
						FString ParamSubtype;
						if (PinInfo->PinType.PinSubCategoryObject.IsValid())
							ParamSubtype = PinInfo->PinType.PinSubCategoryObject->GetName();

						if (MatchesType(ParamSubtype) || MatchesType(PinInfo->PinType.PinCategory.ToString()))
						{
							TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
							R->SetStringField(TEXT("blueprint"), BPName);
							R->SetStringField(TEXT("blueprintPath"), Path);
							R->SetStringField(TEXT("usage"), TEXT("functionParameter"));
							R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
								*Graph->GetName(), *PinInfo->PinName.ToString()));
							R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							R->SetStringField(TEXT("currentType"), PinInfo->PinType.PinCategory.ToString());
							if (!ParamSubtype.IsEmpty())
								R->SetStringField(TEXT("currentSubtype"), ParamSubtype);
							Results.Add(MakeShared<FJsonValueObject>(R));
						}
					}
				}
				else if (auto* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					for (const TSharedPtr<FUserPinInfo>& PinInfo : CustomEvent->UserDefinedPins)
					{
						if (!PinInfo.IsValid()) continue;
						FString ParamSubtype;
						if (PinInfo->PinType.PinSubCategoryObject.IsValid())
							ParamSubtype = PinInfo->PinType.PinSubCategoryObject->GetName();

						if (MatchesType(ParamSubtype) || MatchesType(PinInfo->PinType.PinCategory.ToString()))
						{
							TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
							R->SetStringField(TEXT("blueprint"), BPName);
							R->SetStringField(TEXT("blueprintPath"), Path);
							R->SetStringField(TEXT("usage"), TEXT("eventParameter"));
							R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
								*CustomEvent->CustomFunctionName.ToString(), *PinInfo->PinName.ToString()));
							R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							R->SetStringField(TEXT("currentType"), PinInfo->PinType.PinCategory.ToString());
							if (!ParamSubtype.IsEmpty())
								R->SetStringField(TEXT("currentSubtype"), ParamSubtype);
							Results.Add(MakeShared<FJsonValueObject>(R));
						}
					}
				}
				// Check Break/Make struct nodes
				else if (auto* BreakNode = Cast<UK2Node_BreakStruct>(Node))
				{
					if (BreakNode->StructType && MatchesType(BreakNode->StructType->GetName()))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("breakStruct"));
						R->SetStringField(TEXT("location"), Graph->GetName());
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("structType"), BreakNode->StructType->GetName());
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}
				else if (auto* MakeNode = Cast<UK2Node_MakeStruct>(Node))
				{
					if (MakeNode->StructType && MatchesType(MakeNode->StructType->GetName()))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("makeStruct"));
						R->SetStringField(TEXT("location"), Graph->GetName());
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("structType"), MakeNode->StructType->GetName());
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}

				// Check pin connections carrying the type
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->bHidden || Results.Num() >= MaxResults) continue;

					FString PinSubtype;
					if (Pin->PinType.PinSubCategoryObject.IsValid())
						PinSubtype = Pin->PinType.PinSubCategoryObject->GetName();

					if (Pin->LinkedTo.Num() > 0 &&
						(MatchesType(PinSubtype) || MatchesType(Pin->PinType.PinCategory.ToString())))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("pinConnection"));
						R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
							*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
							*Pin->PinName.ToString()));
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("graph"), Graph->GetName());
						R->SetStringField(TEXT("pinType"), Pin->PinType.PinCategory.ToString());
						if (!PinSubtype.IsEmpty())
							R->SetStringField(TEXT("pinSubtype"), PinSubtype);
						R->SetNumberField(TEXT("connectionCount"), Pin->LinkedTo.Num());
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("typeName"), TypeName);
	Result->SetNumberField(TEXT("resultCount"), Results.Num());
	Result->SetArrayField(TEXT("results"), Results);
	return JsonToString(Result);
}

// ============================================================
// Log capture device for intercepting UE_LOG output during compilation
// ============================================================

class FCompileLogCapture : public FOutputDevice
{
public:
	// Patterns that indicate real compilation problems (not routine info)
	TArray<FString> CapturedErrors;
	TArray<FString> CapturedWarnings;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FString Msg(V);

		// Capture error-level messages
		if (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Fatal)
		{
			CapturedErrors.Add(Msg);
			return;
		}

		// Capture warning-level messages
		if (Verbosity == ELogVerbosity::Warning)
		{
			// Filter out our own BlueprintMCP messages to avoid noise
			if (!Msg.Contains(TEXT("BlueprintMCP:")))
			{
				CapturedWarnings.Add(Msg);
			}
			return;
		}

		// Also capture Display/Log messages that contain known error patterns
		// These are emitted by UE5's Blueprint compiler at Log level, not Warning/Error
		static const TCHAR* ErrorPatterns[] = {
			TEXT("Can't connect pins"),
			TEXT("Fixed up function"),
			TEXT("is not compatible with"),
			TEXT("could not find a pin"),
			TEXT("has an invalid"),
			TEXT("orphaned pin"),
			TEXT("is deprecated"),
			TEXT("does not implement"),
			TEXT("Missing function"),
			TEXT("Unable to find"),
			TEXT("Failed to resolve"),
		};

		for (const TCHAR* Pattern : ErrorPatterns)
		{
			if (Msg.Contains(Pattern))
			{
				CapturedWarnings.Add(Msg);
				return;
			}
		}
	}
};

// Helper: validate a single Blueprint and return structured JSON result
static TSharedRef<FJsonObject> ValidateSingleBlueprint(UBlueprint* BP, const FString& BlueprintName)
{
	// Start log capture before compilation
	FCompileLogCapture LogCapture;
	GLog->AddOutputDevice(&LogCapture);

	// Compile with SkipSave so we don't persist anything
	EBlueprintCompileOptions CompileOpts =
		EBlueprintCompileOptions::SkipSave |
		EBlueprintCompileOptions::SkipGarbageCollection |
		EBlueprintCompileOptions::SkipFiBSearchMetaUpdate;

	bool bCompileCrashed = false;

#if PLATFORM_WINDOWS
	int32 CompileResult = TryCompileBlueprintSEH(BP, CompileOpts);
	if (CompileResult != 0)
	{
		bCompileCrashed = true;
	}
#else
	FKismetEditorUtilities::CompileBlueprint(BP, CompileOpts, nullptr);
#endif

	// Stop log capture
	GLog->RemoveOutputDevice(&LogCapture);

	// Collect errors and warnings from node compiler messages
	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->bHasCompilerMessage)
			{
				TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
				Msg->SetStringField(TEXT("graph"), Graph->GetName());
				Msg->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
				Msg->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				Msg->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
				Msg->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType == EMessageSeverity::Error)
				{
					Msg->SetStringField(TEXT("severity"), TEXT("error"));
					ErrorsArr.Add(MakeShared<FJsonValueObject>(Msg));
				}
				else
				{
					Msg->SetStringField(TEXT("severity"), TEXT("warning"));
					WarningsArr.Add(MakeShared<FJsonValueObject>(Msg));
				}
			}
		}
	}

	// Add captured log errors
	for (const FString& LogErr : LogCapture.CapturedErrors)
	{
		TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("source"), TEXT("log"));
		Msg->SetStringField(TEXT("message"), LogErr);
		Msg->SetStringField(TEXT("severity"), TEXT("error"));
		ErrorsArr.Add(MakeShared<FJsonValueObject>(Msg));
	}

	// Add captured log warnings
	for (const FString& LogWarn : LogCapture.CapturedWarnings)
	{
		TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("source"), TEXT("log"));
		Msg->SetStringField(TEXT("message"), LogWarn);
		Msg->SetStringField(TEXT("severity"), TEXT("warning"));
		WarningsArr.Add(MakeShared<FJsonValueObject>(Msg));
	}

	FString StatusStr;
	switch (BP->Status)
	{
		case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
		case BS_Dirty: StatusStr = TEXT("Dirty"); break;
		case BS_Error: StatusStr = TEXT("Error"); break;
		case BS_Unknown: StatusStr = TEXT("Unknown"); break;
		default: StatusStr = FString::Printf(TEXT("Status_%d"), (int32)BP->Status); break;
	}

	bool bIsValid = (BP->Status == BS_UpToDate) && ErrorsArr.Num() == 0;

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("status"), StatusStr);
	Result->SetBoolField(TEXT("isValid"), bIsValid);
	Result->SetNumberField(TEXT("errorCount"), ErrorsArr.Num());
	Result->SetArrayField(TEXT("errors"), ErrorsArr);
	Result->SetNumberField(TEXT("warningCount"), WarningsArr.Num());
	Result->SetArrayField(TEXT("warnings"), WarningsArr);
	if (bCompileCrashed)
	{
		Result->SetStringField(TEXT("compileWarning"), TEXT("Compilation crashed (SEH caught), results may be incomplete"));
	}
	return Result;
}

// HandleValidateBlueprint — compile without saving, report errors + captured log messages
// ============================================================

FString FBlueprintMCPServer::HandleValidateBlueprint(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	if (BlueprintName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: blueprint"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Validating blueprint '%s'"), *BlueprintName);

	TSharedRef<FJsonObject> Result = ValidateSingleBlueprint(BP, BlueprintName);
	return JsonToString(Result);
}

// ============================================================
// HandleValidateAllBlueprints — bulk validation
// ============================================================

FString FBlueprintMCPServer::HandleValidateAllBlueprints(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	// Body is optional — empty body means validate all

	FString Filter;
	if (Json.IsValid())
	{
		Filter = Json->GetStringField(TEXT("filter"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Bulk validating blueprints (filter: '%s')"),
		Filter.IsEmpty() ? TEXT("*") : *Filter);

	TArray<TSharedPtr<FJsonValue>> FailedArr;
	int32 TotalChecked = 0;
	int32 TotalPassed = 0;
	int32 TotalFailed = 0;
	int32 TotalCrashed = 0;

	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		FString AssetName = Asset.AssetName.ToString();
		FString PackagePath = Asset.PackageName.ToString();

		// Apply filter if specified
		if (!Filter.IsEmpty())
		{
			if (!PackagePath.Contains(Filter) && !AssetName.Contains(Filter))
			{
				continue;
			}
		}

		// Load the Blueprint
		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			continue;
		}

		TotalChecked++;

		TSharedRef<FJsonObject> Result = ValidateSingleBlueprint(BP, AssetName);

		bool bValid = Result->GetBoolField(TEXT("isValid"));
		int32 Errors = (int32)Result->GetNumberField(TEXT("errorCount"));
		int32 Warnings = (int32)Result->GetNumberField(TEXT("warningCount"));

		if (Result->HasField(TEXT("compileWarning")))
		{
			TotalCrashed++;
		}

		if (bValid && Errors == 0)
		{
			TotalPassed++;
		}
		else
		{
			TotalFailed++;
			// Include path for context in bulk results
			Result->SetStringField(TEXT("path"), PackagePath);
			FailedArr.Add(MakeShared<FJsonValueObject>(Result));
		}

		// Log progress every 50 blueprints
		if (TotalChecked % 50 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Validated %d blueprints so far (%d failed)..."),
				TotalChecked, TotalFailed);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Bulk validation complete — %d checked, %d passed, %d failed, %d crashed"),
		TotalChecked, TotalPassed, TotalFailed, TotalCrashed);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("totalChecked"), TotalChecked);
	Result->SetNumberField(TEXT("totalPassed"), TotalPassed);
	Result->SetNumberField(TEXT("totalFailed"), TotalFailed);
	if (TotalCrashed > 0)
	{
		Result->SetNumberField(TEXT("totalCrashed"), TotalCrashed);
	}
	Result->SetArrayField(TEXT("failed"), FailedArr);
	if (!Filter.IsEmpty())
	{
		Result->SetStringField(TEXT("filter"), Filter);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleAddNode — create a new node in a blueprint graph
// ============================================================

FString FBlueprintMCPServer::HandleAddNode(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString GraphName = Json->GetStringField(TEXT("graph"));
	FString NodeType = Json->GetStringField(TEXT("nodeType"));

	if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || NodeType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, graph, nodeType"));
	}

	int32 PosX = 0, PosY = 0;
	if (Json->HasField(TEXT("posX")))
		PosX = (int32)Json->GetNumberField(TEXT("posX"));
	if (Json->HasField(TEXT("posY")))
		PosY = (int32)Json->GetNumberField(TEXT("posY"));

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the target graph (URL decode graph name)
	FString DecodedGraphName = UrlDecode(GraphName);
	UEdGraph* TargetGraph = nullptr;
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(DecodedGraphName, ESearchCase::IgnoreCase))
		{
			TargetGraph = Graph;
			break;
		}
	}

	if (!TargetGraph)
	{
		TArray<TSharedPtr<FJsonValue>> GraphNames;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph) GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph '%s' not found"), *DecodedGraphName));
		E->SetArrayField(TEXT("availableGraphs"), GraphNames);
		return JsonToString(E);
	}

	UEdGraphNode* NewNode = nullptr;

	if (NodeType == TEXT("BreakStruct") || NodeType == TEXT("MakeStruct"))
	{
		FString TypeName = Json->GetStringField(TEXT("typeName"));
		if (TypeName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'typeName' for BreakStruct/MakeStruct"));
		}

		// Find the struct type
		FString SearchName = TypeName;
		if (SearchName.StartsWith(TEXT("F")))
			SearchName = SearchName.Mid(1);

		UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*SearchName);
		if (!FoundStruct)
			FoundStruct = FindFirstObject<UScriptStruct>(*TypeName);
		if (!FoundStruct)
		{
			// Broader search
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (It->GetName() == SearchName || It->GetName() == TypeName)
				{
					FoundStruct = *It;
					break;
				}
			}
		}
		if (!FoundStruct)
		{
			return MakeErrorJson(FString::Printf(TEXT("Struct type '%s' not found"), *TypeName));
		}

		if (NodeType == TEXT("BreakStruct"))
		{
			UK2Node_BreakStruct* BreakNode = NewObject<UK2Node_BreakStruct>(TargetGraph);
			BreakNode->StructType = FoundStruct;
			BreakNode->NodePosX = PosX;
			BreakNode->NodePosY = PosY;
			TargetGraph->AddNode(BreakNode, false, false);
			BreakNode->AllocateDefaultPins();
			NewNode = BreakNode;
		}
		else
		{
			UK2Node_MakeStruct* MakeNode = NewObject<UK2Node_MakeStruct>(TargetGraph);
			MakeNode->StructType = FoundStruct;
			MakeNode->NodePosX = PosX;
			MakeNode->NodePosY = PosY;
			TargetGraph->AddNode(MakeNode, false, false);
			MakeNode->AllocateDefaultPins();
			NewNode = MakeNode;
		}
	}
	else if (NodeType == TEXT("CallFunction"))
	{
		FString FunctionName = Json->GetStringField(TEXT("functionName"));
		FString ClassName = Json->GetStringField(TEXT("className"));

		if (FunctionName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'functionName' for CallFunction"));
		}

		// Find the function
		UFunction* TargetFunc = nullptr;

		if (!ClassName.IsEmpty())
		{
			// Search in specified class
			UClass* TargetClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == ClassName || It->GetName() == FString::Printf(TEXT("U%s"), *ClassName))
				{
					TargetClass = *It;
					break;
				}
			}
			if (TargetClass)
			{
				TargetFunc = TargetClass->FindFunctionByName(FName(*FunctionName));
			}
		}

		if (!TargetFunc)
		{
			// Search across all classes
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UFunction* Func = It->FindFunctionByName(FName(*FunctionName));
				if (Func)
				{
					TargetFunc = Func;
					break;
				}
			}
		}

		if (!TargetFunc)
		{
			return MakeErrorJson(FString::Printf(TEXT("Function '%s' not found%s"),
				*FunctionName, ClassName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in class '%s'"), *ClassName)));
		}

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(TargetGraph);
		CallNode->SetFromFunction(TargetFunc);
		CallNode->NodePosX = PosX;
		CallNode->NodePosY = PosY;
		TargetGraph->AddNode(CallNode, false, false);
		CallNode->AllocateDefaultPins();
		NewNode = CallNode;
	}
	else if (NodeType == TEXT("VariableGet") || NodeType == TEXT("VariableSet"))
	{
		FString VariableName = Json->GetStringField(TEXT("variableName"));
		if (VariableName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'variableName' for VariableGet/VariableSet"));
		}

		// Verify the variable exists in the blueprint
		FName VarFName(*VariableName);
		bool bVarFound = false;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == VarFName)
			{
				bVarFound = true;
				break;
			}
		}

		if (!bVarFound)
		{
			// Also check inherited properties
			if (BP->GeneratedClass)
			{
				FProperty* Prop = BP->GeneratedClass->FindPropertyByName(VarFName);
				if (Prop)
					bVarFound = true;
			}
		}

		if (!bVarFound)
		{
			return MakeErrorJson(FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"),
				*VariableName, *BlueprintName));
		}

		if (NodeType == TEXT("VariableGet"))
		{
			UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(TargetGraph);
			GetNode->VariableReference.SetSelfMember(VarFName);
			GetNode->NodePosX = PosX;
			GetNode->NodePosY = PosY;
			TargetGraph->AddNode(GetNode, false, false);
			GetNode->AllocateDefaultPins();
			NewNode = GetNode;
		}
		else
		{
			UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(TargetGraph);
			SetNode->VariableReference.SetSelfMember(VarFName);
			SetNode->NodePosX = PosX;
			SetNode->NodePosY = PosY;
			TargetGraph->AddNode(SetNode, false, false);
			SetNode->AllocateDefaultPins();
			NewNode = SetNode;
		}
	}
	else if (NodeType == TEXT("DynamicCast"))
	{
		FString CastTarget = Json->GetStringField(TEXT("castTarget"));
		if (CastTarget.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'castTarget' for DynamicCast"));
		}

		// Find the target class (C++ or Blueprint)
		UClass* TargetClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			FString ClassName = It->GetName();
			if (ClassName == CastTarget || ClassName == CastTarget + TEXT("_C"))
			{
				TargetClass = *It;
				break;
			}
		}
		if (!TargetClass)
		{
			return MakeErrorJson(FString::Printf(TEXT("Cast target class '%s' not found"), *CastTarget));
		}

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(TargetGraph);
		CastNode->TargetType = TargetClass;
		CastNode->NodePosX = PosX;
		CastNode->NodePosY = PosY;
		TargetGraph->AddNode(CastNode, false, false);
		CastNode->AllocateDefaultPins();
		NewNode = CastNode;
	}
	else if (NodeType == TEXT("OverrideEvent"))
	{
		FString FunctionName = Json->GetStringField(TEXT("functionName"));
		if (FunctionName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'functionName' for OverrideEvent"));
		}

		if (!BP->ParentClass)
		{
			return MakeErrorJson(TEXT("Blueprint has no parent class"));
		}

		UFunction* Func = BP->ParentClass->FindFunctionByName(FName(*FunctionName));
		if (!Func)
		{
			return MakeErrorJson(FString::Printf(TEXT("Function '%s' not found on parent class '%s'"),
				*FunctionName, *BP->ParentClass->GetName()));
		}

		// Check for duplicate override event already in graph
		for (UEdGraphNode* ExistingNode : TargetGraph->Nodes)
		{
			if (UK2Node_Event* ExistingEvent = Cast<UK2Node_Event>(ExistingNode))
			{
				if (ExistingEvent->bOverrideFunction &&
					ExistingEvent->EventReference.GetMemberName() == FName(*FunctionName))
				{
					// Already exists — return it with alreadyExists flag
					TSharedPtr<FJsonObject> NodeState = SerializeNode(ExistingEvent);
					TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetBoolField(TEXT("success"), true);
					Result->SetBoolField(TEXT("alreadyExists"), true);
					Result->SetStringField(TEXT("blueprint"), BlueprintName);
					Result->SetStringField(TEXT("graph"), DecodedGraphName);
					Result->SetStringField(TEXT("nodeType"), NodeType);
					Result->SetStringField(TEXT("nodeId"), ExistingEvent->NodeGuid.ToString());
					if (NodeState.IsValid())
					{
						Result->SetObjectField(TEXT("node"), NodeState);
					}

					UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: OverrideEvent '%s' already exists in '%s', returning existing node"),
						*FunctionName, *BlueprintName);
					return JsonToString(Result);
				}
			}
		}

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(TargetGraph);
		EventNode->EventReference.SetFromField<UFunction>(Func, false);
		EventNode->bOverrideFunction = true;
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		TargetGraph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		NewNode = EventNode;
	}
	else if (NodeType == TEXT("CallParentFunction"))
	{
		FString FunctionName = Json->GetStringField(TEXT("functionName"));
		if (FunctionName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'functionName' for CallParentFunction"));
		}

		if (!BP->ParentClass)
		{
			return MakeErrorJson(TEXT("Blueprint has no parent class"));
		}

		UFunction* Func = BP->ParentClass->FindFunctionByName(FName(*FunctionName));
		if (!Func)
		{
			return MakeErrorJson(FString::Printf(TEXT("Function '%s' not found on parent class '%s'"),
				*FunctionName, *BP->ParentClass->GetName()));
		}

		UK2Node_CallParentFunction* ParentCallNode = NewObject<UK2Node_CallParentFunction>(TargetGraph);
		ParentCallNode->SetFromFunction(Func);
		ParentCallNode->NodePosX = PosX;
		ParentCallNode->NodePosY = PosY;
		TargetGraph->AddNode(ParentCallNode, false, false);
		ParentCallNode->AllocateDefaultPins();
		NewNode = ParentCallNode;
	}
	else
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Unsupported nodeType '%s'. Supported: BreakStruct, MakeStruct, CallFunction, VariableGet, VariableSet, DynamicCast, OverrideEvent, CallParentFunction"),
			*NodeType));
	}

	if (!NewNode)
	{
		return MakeErrorJson(TEXT("Failed to create node"));
	}

	// Ensure node has a valid GUID (PostInitProperties may skip it in some contexts)
	if (!NewNode->NodeGuid.IsValid())
	{
		NewNode->CreateNewGuid();
	}

	// Mark as modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added %s node '%s' in graph '%s' of '%s', save %s"),
		*NodeType, *NewNode->NodeGuid.ToString(), *DecodedGraphName, *BlueprintName,
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	// Serialize the new node (includes GUID and pin list)
	TSharedPtr<FJsonObject> NodeState = SerializeNode(NewNode);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("graph"), DecodedGraphName);
	Result->SetStringField(TEXT("nodeType"), NodeType);
	Result->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (NodeState.IsValid())
	{
		Result->SetObjectField(TEXT("node"), NodeState);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleRenameAsset — rename or move an asset
// ============================================================

FString FBlueprintMCPServer::HandleRenameAsset(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	FString NewPath = Json->GetStringField(TEXT("newPath"));

	if (AssetPath.IsEmpty() || NewPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: assetPath, newPath"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Renaming asset '%s' -> '%s'"), *AssetPath, *NewPath);

	// Use FAssetToolsModule to perform the rename with reference fixup
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	// Build the source/dest arrays
	TArray<FAssetRenameData> RenameData;

	// We need to load the asset to get the object
	FAssetData* FoundAsset = FindBlueprintAsset(AssetPath);
	if (!FoundAsset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Asset '%s' not found in indexed blueprints"), *AssetPath));
	}

	UObject* AssetObj = FoundAsset->GetAsset();
	if (!AssetObj)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load asset '%s'"), *AssetPath));
	}

	// Parse new path into package path and asset name
	FString NewPackagePath, NewAssetName;
	int32 LastSlash;
	if (NewPath.FindLastChar(TEXT('/'), LastSlash))
	{
		NewPackagePath = NewPath.Left(LastSlash);
		NewAssetName = NewPath.Mid(LastSlash + 1);
	}
	else
	{
		// If no slash, assume same directory with new name
		FString OldPackagePath;
		if (AssetPath.FindLastChar(TEXT('/'), LastSlash))
		{
			OldPackagePath = AssetPath.Left(LastSlash);
		}
		NewPackagePath = OldPackagePath;
		NewAssetName = NewPath;
	}

	FAssetRenameData RenameEntry(AssetObj, NewPackagePath, NewAssetName);
	RenameData.Add(RenameEntry);

	bool bSuccess = AssetTools.RenameAssets(RenameData);

	if (bSuccess)
	{
		// Update our cached asset list — re-scan to pick up the new path
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		AllBlueprintAssets.Empty();
		ARM.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprintAssets, true);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Rename %s"), bSuccess ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("oldPath"), AssetPath);
	Result->SetStringField(TEXT("newPath"), NewPath);
	Result->SetStringField(TEXT("newPackagePath"), NewPackagePath);
	Result->SetStringField(TEXT("newAssetName"), NewAssetName);
	if (!bSuccess)
	{
		Result->SetStringField(TEXT("error"), TEXT("Asset rename failed. The target path may be invalid or a conflicting asset may exist."));
	}
	return JsonToString(Result);
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
// Snapshot Helpers
// ============================================================

FString FBlueprintMCPServer::GenerateSnapshotId(const FString& BlueprintName)
{
	FString CleanName = BlueprintName;
	CleanName.ReplaceInline(TEXT("/"), TEXT("_"));
	CleanName.ReplaceInline(TEXT(" "), TEXT("_"));
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	return FString::Printf(TEXT("%s_%s_%s"), *CleanName, *Timestamp, *FGuid::NewGuid().ToString().Left(8));
}

FGraphSnapshotData FBlueprintMCPServer::CaptureGraphSnapshot(UEdGraph* Graph)
{
	FGraphSnapshotData Data;
	if (!Graph) return Data;

	// Record all nodes
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		FNodeRecord Record;
		Record.NodeGuid = Node->NodeGuid.ToString();
		Record.NodeClass = Node->GetClass()->GetName();
		Record.NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		// Check for Break/Make struct type
		if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
		{
			Record.StructType = BreakNode->StructType ? BreakNode->StructType->GetName() : TEXT("<unknown struct>");
		}
		else if (UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
		{
			Record.StructType = MakeNode->StructType ? MakeNode->StructType->GetName() : TEXT("<unknown struct>");
		}

		Data.Nodes.Add(Record);

		// Record ALL pin connections (only from output pins to avoid duplicates)
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction != EGPD_Output) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;

				FPinConnectionRecord ConnRecord;
				ConnRecord.SourceNodeGuid = Node->NodeGuid.ToString();
				ConnRecord.SourcePinName = Pin->PinName.ToString();
				ConnRecord.TargetNodeGuid = Linked->GetOwningNode()->NodeGuid.ToString();
				ConnRecord.TargetPinName = Linked->PinName.ToString();
				Data.Connections.Add(ConnRecord);
			}
		}
	}

	return Data;
}

void FBlueprintMCPServer::PruneOldSnapshots()
{
	while (Snapshots.Num() > MaxSnapshots)
	{
		FString OldestId;
		FDateTime OldestTime = FDateTime::MaxValue();

		for (const auto& Pair : Snapshots)
		{
			if (Pair.Value.CreatedAt < OldestTime)
			{
				OldestTime = Pair.Value.CreatedAt;
				OldestId = Pair.Key;
			}
		}

		if (!OldestId.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Pruning old snapshot '%s'"), *OldestId);
			Snapshots.Remove(OldestId);
		}
		else
		{
			break;
		}
	}
}

bool FBlueprintMCPServer::SaveSnapshotToDisk(const FString& SnapshotId, const FGraphSnapshot& Snapshot)
{
	FString Dir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("Snapshots");
	IFileManager::Get().MakeDirectory(*Dir, true);

	FString FilePath = Dir / (SnapshotId + TEXT(".json"));

	// Serialize to JSON
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
	Root->SetStringField(TEXT("blueprintName"), Snapshot.BlueprintName);
	Root->SetStringField(TEXT("blueprintPath"), Snapshot.BlueprintPath);
	Root->SetStringField(TEXT("createdAt"), Snapshot.CreatedAt.ToIso8601());

	TSharedRef<FJsonObject> GraphsObj = MakeShared<FJsonObject>();
	for (const auto& GraphPair : Snapshot.Graphs)
	{
		TSharedRef<FJsonObject> GraphObj = MakeShared<FJsonObject>();

		// Nodes
		TArray<TSharedPtr<FJsonValue>> NodesArr;
		for (const FNodeRecord& NodeRec : GraphPair.Value.Nodes)
		{
			TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
			NJ->SetStringField(TEXT("nodeGuid"), NodeRec.NodeGuid);
			NJ->SetStringField(TEXT("nodeClass"), NodeRec.NodeClass);
			NJ->SetStringField(TEXT("nodeTitle"), NodeRec.NodeTitle);
			if (!NodeRec.StructType.IsEmpty())
			{
				NJ->SetStringField(TEXT("structType"), NodeRec.StructType);
			}
			NodesArr.Add(MakeShared<FJsonValueObject>(NJ));
		}
		GraphObj->SetArrayField(TEXT("nodes"), NodesArr);

		// Connections
		TArray<TSharedPtr<FJsonValue>> ConnsArr;
		for (const FPinConnectionRecord& ConnRec : GraphPair.Value.Connections)
		{
			TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
			CJ->SetStringField(TEXT("sourceNodeGuid"), ConnRec.SourceNodeGuid);
			CJ->SetStringField(TEXT("sourcePinName"), ConnRec.SourcePinName);
			CJ->SetStringField(TEXT("targetNodeGuid"), ConnRec.TargetNodeGuid);
			CJ->SetStringField(TEXT("targetPinName"), ConnRec.TargetPinName);
			ConnsArr.Add(MakeShared<FJsonValueObject>(CJ));
		}
		GraphObj->SetArrayField(TEXT("connections"), ConnsArr);

		GraphsObj->SetObjectField(GraphPair.Key, GraphObj);
	}
	Root->SetObjectField(TEXT("graphs"), GraphsObj);

	FString JsonString = JsonToString(Root);
	bool bSuccess = FFileHelper::SaveStringToFile(JsonString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Saved snapshot to disk: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Failed to save snapshot to disk: %s"), *FilePath);
	}
	return bSuccess;
}

bool FBlueprintMCPServer::LoadSnapshotFromDisk(const FString& SnapshotId, FGraphSnapshot& OutSnapshot)
{
	FString Dir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("Snapshots");
	FString FilePath = Dir / (SnapshotId + TEXT(".json"));

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	OutSnapshot.SnapshotId = Root->GetStringField(TEXT("snapshotId"));
	OutSnapshot.BlueprintName = Root->GetStringField(TEXT("blueprintName"));
	OutSnapshot.BlueprintPath = Root->GetStringField(TEXT("blueprintPath"));
	FDateTime::ParseIso8601(*Root->GetStringField(TEXT("createdAt")), OutSnapshot.CreatedAt);

	const TSharedPtr<FJsonObject>* GraphsObjPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("graphs"), GraphsObjPtr) && GraphsObjPtr && (*GraphsObjPtr).IsValid())
	{
		for (const auto& GraphPair : (*GraphsObjPtr)->Values)
		{
			FGraphSnapshotData GraphData;
			const TSharedPtr<FJsonObject>& GraphObj = GraphPair.Value->AsObject();
			if (!GraphObj.IsValid()) continue;

			// Nodes
			const TArray<TSharedPtr<FJsonValue>>* NodesArrPtr = nullptr;
			if (GraphObj->TryGetArrayField(TEXT("nodes"), NodesArrPtr))
			{
				for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArrPtr)
				{
					const TSharedPtr<FJsonObject>& NJ = NodeVal->AsObject();
					if (!NJ.IsValid()) continue;

					FNodeRecord NodeRec;
					NodeRec.NodeGuid = NJ->GetStringField(TEXT("nodeGuid"));
					NodeRec.NodeClass = NJ->GetStringField(TEXT("nodeClass"));
					NodeRec.NodeTitle = NJ->GetStringField(TEXT("nodeTitle"));
					NJ->TryGetStringField(TEXT("structType"), NodeRec.StructType);
					GraphData.Nodes.Add(NodeRec);
				}
			}

			// Connections
			const TArray<TSharedPtr<FJsonValue>>* ConnsArrPtr = nullptr;
			if (GraphObj->TryGetArrayField(TEXT("connections"), ConnsArrPtr))
			{
				for (const TSharedPtr<FJsonValue>& ConnVal : *ConnsArrPtr)
				{
					const TSharedPtr<FJsonObject>& CJ = ConnVal->AsObject();
					if (!CJ.IsValid()) continue;

					FPinConnectionRecord ConnRec;
					ConnRec.SourceNodeGuid = CJ->GetStringField(TEXT("sourceNodeGuid"));
					ConnRec.SourcePinName = CJ->GetStringField(TEXT("sourcePinName"));
					ConnRec.TargetNodeGuid = CJ->GetStringField(TEXT("targetNodeGuid"));
					ConnRec.TargetPinName = CJ->GetStringField(TEXT("targetPinName"));
					GraphData.Connections.Add(ConnRec);
				}
			}

			OutSnapshot.Graphs.Add(GraphPair.Key, GraphData);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Loaded snapshot from disk: %s"), *FilePath);
	return true;
}

// ============================================================
// HandleSnapshotGraph
// ============================================================

FString FBlueprintMCPServer::HandleSnapshotGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	if (BlueprintName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: blueprint"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating snapshot for blueprint '%s'"), *BlueprintName);

	// Build the snapshot
	FGraphSnapshot Snapshot;
	Snapshot.SnapshotId = GenerateSnapshotId(BlueprintName);
	Snapshot.BlueprintName = BP->GetName();
	Snapshot.BlueprintPath = BP->GetPathName();
	Snapshot.CreatedAt = FDateTime::Now();

	// Gather all graphs (UbergraphPages + FunctionGraphs)
	TArray<UEdGraph*> GraphsToCapture;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		GraphsToCapture.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		GraphsToCapture.Add(Graph);
	}

	if (GraphsToCapture.Num() == 0 && !GraphFilter.IsEmpty())
	{
		return MakeErrorJson(FString::Printf(TEXT("Graph '%s' not found in blueprint '%s'"), *GraphFilter, *BlueprintName));
	}

	int32 TotalConnections = 0;
	TArray<TSharedPtr<FJsonValue>> GraphSummaries;

	for (UEdGraph* Graph : GraphsToCapture)
	{
		FGraphSnapshotData GraphData = CaptureGraphSnapshot(Graph);

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("name"), Graph->GetName());
		Summary->SetNumberField(TEXT("nodeCount"), GraphData.Nodes.Num());
		Summary->SetNumberField(TEXT("connectionCount"), GraphData.Connections.Num());
		GraphSummaries.Add(MakeShared<FJsonValueObject>(Summary));

		TotalConnections += GraphData.Connections.Num();
		Snapshot.Graphs.Add(Graph->GetName(), MoveTemp(GraphData));
	}

	// Store in memory
	Snapshots.Add(Snapshot.SnapshotId, Snapshot);
	PruneOldSnapshots();

	// Save to disk
	SaveSnapshotToDisk(Snapshot.SnapshotId, Snapshot);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Snapshot '%s' created with %d graphs, %d total connections"),
		*Snapshot.SnapshotId, GraphsToCapture.Num(), TotalConnections);

	// Build response
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetArrayField(TEXT("graphs"), GraphSummaries);
	Result->SetNumberField(TEXT("totalConnections"), TotalConnections);
	return JsonToString(Result);
}

// ============================================================
// HandleDiffGraph
// ============================================================

FString FBlueprintMCPServer::HandleDiffGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));
	if (BlueprintName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, snapshotId"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	// Load snapshot from memory or disk
	FGraphSnapshot* SnapshotPtr = Snapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load the current blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Diffing blueprint '%s' against snapshot '%s'"), *BlueprintName, *SnapshotId);

	// Build current state for comparison
	TMap<FString, FGraphSnapshotData> CurrentGraphs;
	TArray<UEdGraph*> AllGraphs;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		CurrentGraphs.Add(Graph->GetName(), CaptureGraphSnapshot(Graph));
	}

	// Build lookup maps for current state
	// Key: "GraphName|SourceGuid|SourcePin|TargetGuid|TargetPin"
	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	// Build node lookup maps: GUID -> NodeRecord
	TMap<FString, const FNodeRecord*> SnapshotNodeMap;
	TMap<FString, const FNodeRecord*> CurrentNodeMap;

	TArray<TSharedPtr<FJsonValue>> SeveredArr;
	TArray<TSharedPtr<FJsonValue>> NewConnsArr;
	TArray<TSharedPtr<FJsonValue>> TypeChangesArr;
	TArray<TSharedPtr<FJsonValue>> MissingNodesArr;

	// Process each graph in the snapshot
	for (const auto& SnapGraphPair : SnapshotPtr->Graphs)
	{
		const FString& GraphName = SnapGraphPair.Key;
		if (!GraphFilter.IsEmpty() && GraphName != GraphFilter) continue;

		const FGraphSnapshotData& SnapData = SnapGraphPair.Value;
		const FGraphSnapshotData* CurDataPtr = CurrentGraphs.Find(GraphName);

		// Build snapshot node map for this graph
		TMap<FString, const FNodeRecord*> SnapNodeLookup;
		for (const FNodeRecord& NR : SnapData.Nodes)
		{
			SnapNodeLookup.Add(NR.NodeGuid, &NR);
		}

		// Build current connection set and node map for this graph
		TSet<FString> CurrentConnSet;
		TMap<FString, const FNodeRecord*> CurNodeLookup;
		if (CurDataPtr)
		{
			for (const FNodeRecord& NR : CurDataPtr->Nodes)
			{
				CurNodeLookup.Add(NR.NodeGuid, &NR);
			}
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
			}
		}

		// Build snapshot connection set
		TSet<FString> SnapConnSet;
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			SnapConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
		}

		// Find severed connections: in snapshot but not in current
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
			if (!CurrentConnSet.Contains(Key))
			{
				TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
				SJ->SetStringField(TEXT("graph"), GraphName);
				SJ->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
				SJ->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
				SJ->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);
				SJ->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);

				// Add node names for readability
				const FNodeRecord** SrcRec = SnapNodeLookup.Find(Conn.SourceNodeGuid);
				if (SrcRec) SJ->SetStringField(TEXT("sourceNodeName"), (*SrcRec)->NodeTitle);
				const FNodeRecord** TgtRec = SnapNodeLookup.Find(Conn.TargetNodeGuid);
				if (TgtRec) SJ->SetStringField(TEXT("targetNodeName"), (*TgtRec)->NodeTitle);

				SeveredArr.Add(MakeShared<FJsonValueObject>(SJ));
			}
		}

		// Find new connections: in current but not in snapshot
		if (CurDataPtr)
		{
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
				if (!SnapConnSet.Contains(Key))
				{
					TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
					NJ->SetStringField(TEXT("graph"), GraphName);
					NJ->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
					NJ->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
					NJ->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);
					NJ->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);

					const FNodeRecord** SrcRec = CurNodeLookup.Find(Conn.SourceNodeGuid);
					if (SrcRec) NJ->SetStringField(TEXT("sourceNodeName"), (*SrcRec)->NodeTitle);
					const FNodeRecord** TgtRec = CurNodeLookup.Find(Conn.TargetNodeGuid);
					if (TgtRec) NJ->SetStringField(TEXT("targetNodeName"), (*TgtRec)->NodeTitle);

					NewConnsArr.Add(MakeShared<FJsonValueObject>(NJ));
				}
			}
		}

		// Find type changes and missing nodes
		for (const FNodeRecord& SnapNode : SnapData.Nodes)
		{
			const FNodeRecord** CurNodePtr = CurNodeLookup.Find(SnapNode.NodeGuid);
			if (!CurNodePtr)
			{
				// Missing node
				TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
				MJ->SetStringField(TEXT("graph"), GraphName);
				MJ->SetStringField(TEXT("nodeGuid"), SnapNode.NodeGuid);
				MJ->SetStringField(TEXT("nodeClass"), SnapNode.NodeClass);
				MJ->SetStringField(TEXT("nodeTitle"), SnapNode.NodeTitle);
				if (!SnapNode.StructType.IsEmpty())
				{
					MJ->SetStringField(TEXT("structType"), SnapNode.StructType);
				}
				MissingNodesArr.Add(MakeShared<FJsonValueObject>(MJ));
			}
			else if (!SnapNode.StructType.IsEmpty())
			{
				// Check for type change on Break/Make nodes
				const FNodeRecord* CurNode = *CurNodePtr;
				if (CurNode->StructType != SnapNode.StructType)
				{
					TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
					TJ->SetStringField(TEXT("graph"), GraphName);
					TJ->SetStringField(TEXT("nodeGuid"), SnapNode.NodeGuid);
					TJ->SetStringField(TEXT("nodeTitle"), SnapNode.NodeTitle);
					TJ->SetStringField(TEXT("oldStructType"), SnapNode.StructType);
					TJ->SetStringField(TEXT("newStructType"), CurNode->StructType);
					TypeChangesArr.Add(MakeShared<FJsonValueObject>(TJ));
				}
			}
		}
	}

	// Build result
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetStringField(TEXT("snapshotId"), SnapshotId);
	Result->SetArrayField(TEXT("severedConnections"), SeveredArr);
	Result->SetArrayField(TEXT("newConnections"), NewConnsArr);
	Result->SetArrayField(TEXT("typeChanges"), TypeChangesArr);
	Result->SetArrayField(TEXT("missingNodes"), MissingNodesArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("severedConnections"), SeveredArr.Num());
	Summary->SetNumberField(TEXT("newConnections"), NewConnsArr.Num());
	Summary->SetNumberField(TEXT("typeChanges"), TypeChangesArr.Num());
	Summary->SetNumberField(TEXT("missingNodes"), MissingNodesArr.Num());
	Result->SetObjectField(TEXT("summary"), Summary);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Diff complete — %d severed, %d new, %d type changes, %d missing nodes"),
		SeveredArr.Num(), NewConnsArr.Num(), TypeChangesArr.Num(), MissingNodesArr.Num());

	return JsonToString(Result);
}

// ============================================================
// HandleRestoreGraph
// ============================================================

FString FBlueprintMCPServer::HandleRestoreGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));
	if (BlueprintName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, snapshotId"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	FString NodeIdFilter;
	Json->TryGetStringField(TEXT("nodeId"), NodeIdFilter);

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load snapshot from memory or disk
	FGraphSnapshot* SnapshotPtr = Snapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load the current blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Restoring connections from snapshot '%s' for blueprint '%s' (dryRun=%s)"),
		*SnapshotId, *BlueprintName, bDryRun ? TEXT("true") : TEXT("false"));

	// Build current connection set for comparison
	TMap<FString, FGraphSnapshotData> CurrentGraphs;
	TArray<UEdGraph*> AllGraphs;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : AllGraphs)
	{
		CurrentGraphs.Add(Graph->GetName(), CaptureGraphSnapshot(Graph));
	}

	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	int32 Reconnected = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> DetailsArr;

	for (const auto& SnapGraphPair : SnapshotPtr->Graphs)
	{
		const FString& GraphName = SnapGraphPair.Key;
		if (!GraphFilter.IsEmpty() && GraphName != GraphFilter) continue;

		const FGraphSnapshotData& SnapData = SnapGraphPair.Value;
		const FGraphSnapshotData* CurDataPtr = CurrentGraphs.Find(GraphName);

		// Build current connection set
		TSet<FString> CurrentConnSet;
		if (CurDataPtr)
		{
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
			}
		}

		// Find severed connections and try to restore them
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
			if (CurrentConnSet.Contains(Key)) continue; // Still connected, skip

			// Apply nodeId filter if specified
			if (!NodeIdFilter.IsEmpty() && Conn.SourceNodeGuid != NodeIdFilter && Conn.TargetNodeGuid != NodeIdFilter)
			{
				continue;
			}

			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("graph"), GraphName);
			Detail->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
			Detail->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);
			Detail->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
			Detail->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);

			// Find source and target nodes
			UEdGraph* SourceGraph = nullptr;
			UEdGraphNode* SourceNode = FindNodeByGuid(BP, Conn.SourceNodeGuid, &SourceGraph);
			UEdGraphNode* TargetNode = FindNodeByGuid(BP, Conn.TargetNodeGuid);

			if (!SourceNode)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Source node '%s' no longer exists"), *Conn.SourceNodeGuid));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}
			if (!TargetNode)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Target node '%s' no longer exists"), *Conn.TargetNodeGuid));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			Detail->SetStringField(TEXT("sourceNodeName"), SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			Detail->SetStringField(TEXT("targetNodeName"), TargetNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

			// Find pins
			UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*Conn.SourcePinName));
			UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*Conn.TargetPinName));

			if (!SourcePin)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Source pin '%s' not found on node"), *Conn.SourcePinName));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}
			if (!TargetPin)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Target pin '%s' not found on node"), *Conn.TargetPinName));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			if (bDryRun)
			{
				// In dry run, just report what would happen
				Detail->SetStringField(TEXT("result"), TEXT("would_reconnect"));
				Reconnected++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			// Try type-validated connection via the schema
			const UEdGraphSchema* Schema = SourceGraph ? SourceGraph->GetSchema() : nullptr;
			if (!Schema)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), TEXT("Graph schema not found"));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);
			if (bConnected)
			{
				Detail->SetStringField(TEXT("result"), TEXT("reconnected"));
				Reconnected++;
			}
			else
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(
					TEXT("TryCreateConnection failed — types may be incompatible (%s -> %s)"),
					*SourcePin->PinType.PinCategory.ToString(),
					*TargetPin->PinType.PinCategory.ToString()));
				Failed++;
			}
			DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
		}
	}

	// Save if not dry run and we reconnected something
	bool bSaved = false;
	if (!bDryRun && Reconnected > 0)
	{
		bSaved = SaveBlueprintPackage(BP);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Restore complete — %d reconnected, %d failed, saved=%s"),
		Reconnected, Failed, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetNumberField(TEXT("reconnected"), Reconnected);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("details"), DetailsArr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	return JsonToString(Result);
}

// ============================================================
// HandleFindDisconnectedPins
// ============================================================

FString FBlueprintMCPServer::HandleFindDisconnectedPins(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName;
	Json->TryGetStringField(TEXT("blueprint"), BlueprintName);

	FString PathFilter;
	Json->TryGetStringField(TEXT("filter"), PathFilter);

	FString SnapshotId;
	Json->TryGetStringField(TEXT("snapshotId"), SnapshotId);

	if (BlueprintName.IsEmpty() && PathFilter.IsEmpty() && SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Provide at least one of: blueprint, filter, or snapshotId"));
	}

	// Optionally load snapshot for definite-break detection
	FGraphSnapshot* SnapshotPtr = nullptr;
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotId.IsEmpty())
	{
		SnapshotPtr = Snapshots.Find(SnapshotId);
		if (!SnapshotPtr)
		{
			if (LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
			{
				SnapshotPtr = &LoadedSnapshot;
			}
		}
	}

	// Build snapshot connection lookup: "nodeGuid|pinName" -> array of connected targets
	TMap<FString, TArray<FPinConnectionRecord>> SnapConnByNode;
	if (SnapshotPtr)
	{
		for (const auto& GraphPair : SnapshotPtr->Graphs)
		{
			for (const FPinConnectionRecord& Conn : GraphPair.Value.Connections)
			{
				FString SrcKey = FString::Printf(TEXT("%s|%s"), *Conn.SourceNodeGuid, *Conn.SourcePinName);
				SnapConnByNode.FindOrAdd(SrcKey).Add(Conn);
				FString TgtKey = FString::Printf(TEXT("%s|%s"), *Conn.TargetNodeGuid, *Conn.TargetPinName);
				SnapConnByNode.FindOrAdd(TgtKey).Add(Conn);
			}
		}
	}

	// Collect blueprints to scan
	TArray<FString> BlueprintsToScan;
	if (!BlueprintName.IsEmpty())
	{
		BlueprintsToScan.Add(BlueprintName);
	}
	else if (!PathFilter.IsEmpty())
	{
		for (const FAssetData& Asset : AllBlueprintAssets)
		{
			if (Asset.PackageName.ToString().Contains(PathFilter) || Asset.AssetName.ToString().Contains(PathFilter))
			{
				BlueprintsToScan.Add(Asset.AssetName.ToString());
			}
		}
	}
	else if (SnapshotPtr)
	{
		// Use the snapshot's blueprint
		BlueprintsToScan.Add(SnapshotPtr->BlueprintName);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Finding disconnected pins across %d blueprint(s)"), BlueprintsToScan.Num());

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	int32 HighCount = 0;
	int32 MediumCount = 0;
	int32 BlueprintsScanned = 0;

	for (const FString& BPName : BlueprintsToScan)
	{
		FString LoadError;
		UBlueprint* BP = LoadBlueprintByName(BPName, LoadError);
		if (!BP) continue;
		BlueprintsScanned++;

		TArray<UEdGraph*> AllGraphs;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph) AllGraphs.Add(Graph);
		}
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph) AllGraphs.Add(Graph);
		}

		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node);
				UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node);

				// Only inspect Break/Make struct nodes for heuristic detection
				bool bIsBreakMakeNode = (BreakNode || MakeNode);

				FString StructTypeName;
				if (BreakNode)
				{
					StructTypeName = BreakNode->StructType ? BreakNode->StructType->GetName() : TEXT("<unknown struct>");
				}
				else if (MakeNode)
				{
					StructTypeName = MakeNode->StructType ? MakeNode->StructType->GetName() : TEXT("<unknown struct>");
				}

				// Heuristic detection for Break/Make nodes
				if (bIsBreakMakeNode)
				{
					bool bUnresolvedStruct = StructTypeName.Contains(TEXT("unknown")) ||
						StructTypeName.Equals(TEXT("None")) || StructTypeName.IsEmpty();

					if (bUnresolvedStruct)
					{
						// HIGH confidence: unresolved struct
						TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("blueprint"), BP->GetName());
						Item->SetStringField(TEXT("graph"), Graph->GetName());
						Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						Item->SetStringField(TEXT("structType"), StructTypeName);
						Item->SetStringField(TEXT("confidence"), TEXT("HIGH"));
						Item->SetStringField(TEXT("reason"), TEXT("Unresolved or unknown struct type"));

						// List pins
						TArray<TSharedPtr<FJsonValue>> PinsArr;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
							PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
							PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

							// Check snapshot for what it was connected to
							FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
							if (SnapConnByNode.Contains(PinKey))
							{
								TArray<TSharedPtr<FJsonValue>> WasConnArr;
								for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
								{
									FString OtherGuid = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetNodeGuid : CR.SourceNodeGuid;
									FString OtherPin = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetPinName : CR.SourcePinName;
									WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPin)));
								}
								PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
							}
							PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
						}
						Item->SetArrayField(TEXT("pins"), PinsArr);
						ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
						HighCount++;
					}
					else
					{
						// Check for MEDIUM: valid struct but zero data pin connections
						bool bHasDataConnection = false;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							if (Pin->LinkedTo.Num() > 0)
							{
								bHasDataConnection = true;
								break;
							}
						}

						if (!bHasDataConnection)
						{
							TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("blueprint"), BP->GetName());
							Item->SetStringField(TEXT("graph"), Graph->GetName());
							Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
							Item->SetStringField(TEXT("structType"), StructTypeName);
							Item->SetStringField(TEXT("confidence"), TEXT("MEDIUM"));
							Item->SetStringField(TEXT("reason"), TEXT("Break/Make node with valid struct but zero data pin connections"));

							TArray<TSharedPtr<FJsonValue>> PinsArr;
							for (UEdGraphPin* Pin : Node->Pins)
							{
								if (!Pin || Pin->bHidden) continue;
								if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
								TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
								PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
								PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

								FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
								if (SnapConnByNode.Contains(PinKey))
								{
									TArray<TSharedPtr<FJsonValue>> WasConnArr;
									for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
									{
										FString OtherGuid = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetNodeGuid : CR.SourceNodeGuid;
										FString OtherPin = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetPinName : CR.SourcePinName;
										WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPin)));
									}
									PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
								}
								PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
							}
							Item->SetArrayField(TEXT("pins"), PinsArr);
							ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
							MediumCount++;
						}
					}
				}

				// Snapshot-based definite-break detection (applies to ALL node types)
				if (SnapshotPtr)
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->bHidden) continue;
						FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
						if (!SnapConnByNode.Contains(PinKey)) continue;

						// This pin had connections in the snapshot — check if any are now missing
						for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
						{
							// Determine which side is "other"
							bool bWeAreSource = (CR.SourceNodeGuid == Node->NodeGuid.ToString() && CR.SourcePinName == Pin->PinName.ToString());
							FString OtherGuid = bWeAreSource ? CR.TargetNodeGuid : CR.SourceNodeGuid;
							FString OtherPinName = bWeAreSource ? CR.TargetPinName : CR.SourcePinName;

							// Check if this connection still exists
							bool bStillConnected = false;
							for (UEdGraphPin* Linked : Pin->LinkedTo)
							{
								if (Linked && Linked->GetOwningNode() &&
									Linked->GetOwningNode()->NodeGuid.ToString() == OtherGuid &&
									Linked->PinName.ToString() == OtherPinName)
								{
									bStillConnected = true;
									break;
								}
							}

							if (!bStillConnected)
							{
								// Skip if we already reported this node above via heuristic
								// Only add for non-Break/Make nodes, or if the Break/Make node wasn't caught by heuristics
								if (!bIsBreakMakeNode)
								{
									TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
									Item->SetStringField(TEXT("blueprint"), BP->GetName());
									Item->SetStringField(TEXT("graph"), Graph->GetName());
									Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
									Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
									if (!StructTypeName.IsEmpty())
									{
										Item->SetStringField(TEXT("structType"), StructTypeName);
									}
									Item->SetStringField(TEXT("confidence"), TEXT("HIGH"));
									Item->SetStringField(TEXT("reason"), TEXT("Connection existed in snapshot but is now missing"));

									TArray<TSharedPtr<FJsonValue>> PinsArr;
									TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
									PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
									PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
									TArray<TSharedPtr<FJsonValue>> WasConnArr;
									WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPinName)));
									PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
									PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
									Item->SetArrayField(TEXT("pins"), PinsArr);

									ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
									HighCount++;
								}
								break; // Only report once per node per snapshot-check pass
							}
						}
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: FindDisconnectedPins complete — %d HIGH, %d MEDIUM, %d total across %d blueprints"),
		HighCount, MediumCount, ResultsArr.Num(), BlueprintsScanned);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), ResultsArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("high"), HighCount);
	Summary->SetNumberField(TEXT("medium"), MediumCount);
	Summary->SetNumberField(TEXT("total"), ResultsArr.Num());
	Summary->SetNumberField(TEXT("blueprintsScanned"), BlueprintsScanned);
	Result->SetObjectField(TEXT("summary"), Summary);

	return JsonToString(Result);
}

// ============================================================
// HandleAnalyzeRebuildImpact
// ============================================================

FString FBlueprintMCPServer::HandleAnalyzeRebuildImpact(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString ModuleName = Json->GetStringField(TEXT("moduleName"));
	if (ModuleName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: moduleName"));
	}

	// Optional struct name filter
	TArray<FString> StructNameFilter;
	const TArray<TSharedPtr<FJsonValue>>* StructNamesArr = nullptr;
	if (Json->TryGetArrayField(TEXT("structNames"), StructNamesArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *StructNamesArr)
		{
			FString Name = Val->AsString();
			if (!Name.IsEmpty())
			{
				StructNameFilter.Add(Name);
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Analyzing rebuild impact for module '%s'"), *ModuleName);

	// Enumerate all UScriptStruct and UEnum objects belonging to this module
	TArray<UScriptStruct*> FoundStructs;
	TArray<UEnum*> FoundEnums;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (!Struct) continue;

		FString PackageName = Struct->GetOutermost()->GetName();
		if (!PackageName.Contains(ModuleName)) continue;

		// Apply name filter if provided
		if (StructNameFilter.Num() > 0)
		{
			bool bMatch = false;
			for (const FString& FilterName : StructNameFilter)
			{
				// Match with or without F prefix
				FString CleanFilter = FilterName;
				if (CleanFilter.StartsWith(TEXT("F")))
				{
					CleanFilter = CleanFilter.Mid(1);
				}
				if (Struct->GetName() == FilterName || Struct->GetName() == CleanFilter)
				{
					bMatch = true;
					break;
				}
			}
			if (!bMatch) continue;
		}

		FoundStructs.Add(Struct);
	}

	for (TObjectIterator<UEnum> It; It; ++It)
	{
		UEnum* Enum = *It;
		if (!Enum) continue;

		FString PackageName = Enum->GetOutermost()->GetName();
		if (!PackageName.Contains(ModuleName)) continue;

		if (StructNameFilter.Num() > 0)
		{
			bool bMatch = false;
			for (const FString& FilterName : StructNameFilter)
			{
				FString CleanFilter = FilterName;
				if (CleanFilter.StartsWith(TEXT("E")))
				{
					CleanFilter = CleanFilter.Mid(1);
				}
				if (Enum->GetName() == FilterName || Enum->GetName() == CleanFilter)
				{
					bMatch = true;
					break;
				}
			}
			if (!bMatch) continue;
		}

		FoundEnums.Add(Enum);
	}

	// Build list of found types
	TArray<TSharedPtr<FJsonValue>> TypesFoundArr;
	for (UScriptStruct* S : FoundStructs)
	{
		TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
		TJ->SetStringField(TEXT("name"), S->GetName());
		TJ->SetStringField(TEXT("kind"), TEXT("struct"));
		TJ->SetStringField(TEXT("package"), S->GetOutermost()->GetName());
		TypesFoundArr.Add(MakeShared<FJsonValueObject>(TJ));
	}
	for (UEnum* E : FoundEnums)
	{
		TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
		TJ->SetStringField(TEXT("name"), E->GetName());
		TJ->SetStringField(TEXT("kind"), TEXT("enum"));
		TJ->SetStringField(TEXT("package"), E->GetOutermost()->GetName());
		TypesFoundArr.Add(MakeShared<FJsonValueObject>(TJ));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Found %d structs and %d enums in module '%s'"),
		FoundStructs.Num(), FoundEnums.Num(), *ModuleName);

	// Build a set of type names for fast lookup
	TSet<FString> TypeNameSet;
	for (UScriptStruct* S : FoundStructs) TypeNameSet.Add(S->GetName());
	for (UEnum* E : FoundEnums) TypeNameSet.Add(E->GetName());

	// Scan all blueprints for references
	struct FBlueprintImpact
	{
		FString Name;
		FString Path;
		int32 BreakNodes = 0;
		int32 MakeNodes = 0;
		int32 Variables = 0;
		int32 FunctionParams = 0;
		int32 ConnectionsAtRisk = 0;
		FString Risk;
	};

	TArray<FBlueprintImpact> AffectedBlueprints;
	int32 TotalBreakMakeNodes = 0;
	int32 TotalConnectionsAtRisk = 0;

	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		FString LoadError;
		UBlueprint* BP = LoadBlueprintByName(Asset.AssetName.ToString(), LoadError);
		if (!BP) continue;

		FBlueprintImpact Impact;
		Impact.Name = BP->GetName();
		Impact.Path = BP->GetPathName();

		// Scan graphs for Break/Make struct nodes
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
				{
					if (BreakNode->StructType && TypeNameSet.Contains(BreakNode->StructType->GetName()))
					{
						Impact.BreakNodes++;
						// Count data connections at risk
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							Impact.ConnectionsAtRisk += Pin->LinkedTo.Num();
						}
					}
				}
				else if (UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
				{
					if (MakeNode->StructType && TypeNameSet.Contains(MakeNode->StructType->GetName()))
					{
						Impact.MakeNodes++;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							Impact.ConnectionsAtRisk += Pin->LinkedTo.Num();
						}
					}
				}
			}
		}

		// Scan variables
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarType.PinSubCategoryObject.IsValid())
			{
				FString SubObjName = Var.VarType.PinSubCategoryObject->GetName();
				if (TypeNameSet.Contains(SubObjName))
				{
					Impact.Variables++;
				}
			}
		}

		// Scan function parameters
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node);
				if (!FuncEntry) continue;

				for (const TSharedPtr<FUserPinInfo>& PinInfo : FuncEntry->UserDefinedPins)
				{
					if (!PinInfo.IsValid()) continue;
					if (PinInfo->PinType.PinSubCategoryObject.IsValid())
					{
						FString SubObjName = PinInfo->PinType.PinSubCategoryObject->GetName();
						if (TypeNameSet.Contains(SubObjName))
						{
							Impact.FunctionParams++;
						}
					}
				}
			}
		}

		// Only include if this BP is affected
		if (Impact.BreakNodes > 0 || Impact.MakeNodes > 0 || Impact.Variables > 0 || Impact.FunctionParams > 0)
		{
			// Classify risk
			if (Impact.BreakNodes > 0 || Impact.MakeNodes > 0)
			{
				Impact.Risk = TEXT("HIGH");
			}
			else if (Impact.Variables > 0)
			{
				Impact.Risk = TEXT("MEDIUM");
			}
			else
			{
				Impact.Risk = TEXT("LOW");
			}

			TotalBreakMakeNodes += Impact.BreakNodes + Impact.MakeNodes;
			TotalConnectionsAtRisk += Impact.ConnectionsAtRisk;
			AffectedBlueprints.Add(Impact);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Rebuild impact analysis complete — %d affected blueprints, %d Break/Make nodes, %d connections at risk"),
		AffectedBlueprints.Num(), TotalBreakMakeNodes, TotalConnectionsAtRisk);

	// Build response
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("moduleName"), ModuleName);
	Result->SetArrayField(TEXT("typesFound"), TypesFoundArr);

	TArray<TSharedPtr<FJsonValue>> AffectedArr;
	for (const FBlueprintImpact& Impact : AffectedBlueprints)
	{
		TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
		BJ->SetStringField(TEXT("name"), Impact.Name);
		BJ->SetStringField(TEXT("path"), Impact.Path);
		BJ->SetNumberField(TEXT("breakNodes"), Impact.BreakNodes);
		BJ->SetNumberField(TEXT("makeNodes"), Impact.MakeNodes);
		BJ->SetNumberField(TEXT("variables"), Impact.Variables);
		BJ->SetNumberField(TEXT("functionParams"), Impact.FunctionParams);
		BJ->SetNumberField(TEXT("connectionsAtRisk"), Impact.ConnectionsAtRisk);
		BJ->SetStringField(TEXT("risk"), Impact.Risk);
		AffectedArr.Add(MakeShared<FJsonValueObject>(BJ));
	}
	Result->SetArrayField(TEXT("affectedBlueprints"), AffectedArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("totalBlueprints"), AffectedBlueprints.Num());
	Summary->SetNumberField(TEXT("totalBreakMakeNodes"), TotalBreakMakeNodes);
	Summary->SetNumberField(TEXT("totalConnectionsAtRisk"), TotalConnectionsAtRisk);
	Result->SetObjectField(TEXT("summary"), Summary);

	return JsonToString(Result);
}

// ============================================================
// HandleReparentBlueprint — change a Blueprint's parent class
// ============================================================

FString FBlueprintMCPServer::HandleReparentBlueprint(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString NewParentName = Json->GetStringField(TEXT("newParentClass"));

	if (BlueprintName.IsEmpty() || NewParentName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, newParentClass"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	FString OldParentName = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

	// Find the new parent class
	// Try C++ class first (e.g. "WebUIHUD" finds /Script/ModuleName.WebUIHUD)
	UClass* NewParentClass = nullptr;

	// Search across all packages for native classes
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == NewParentName)
		{
			NewParentClass = *It;
			break;
		}
	}

	// If not found as C++ class, try loading as a Blueprint asset
	if (!NewParentClass)
	{
		FString ParentLoadError;
		UBlueprint* ParentBP = LoadBlueprintByName(NewParentName, ParentLoadError);
		if (ParentBP && ParentBP->GeneratedClass)
		{
			NewParentClass = ParentBP->GeneratedClass;
		}
	}

	if (!NewParentClass)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Could not find class '%s'. Provide a C++ class name (e.g. 'WebUIHUD') or Blueprint name."),
			*NewParentName));
	}

	// Validate: new parent must be compatible
	if (BP->ParentClass && !NewParentClass->IsChildOf(BP->ParentClass->GetSuperClass()) &&
		BP->ParentClass != NewParentClass)
	{
		// Just warn, don't block — the user may intentionally reparent to a sibling
		UE_LOG(LogTemp, Warning,
			TEXT("BlueprintMCP: Reparenting '%s' from '%s' to '%s' — classes are not in a direct hierarchy"),
			*BlueprintName, *OldParentName, *NewParentClass->GetName());
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Reparenting '%s' from '%s' to '%s'"),
		*BlueprintName, *OldParentName, *NewParentClass->GetName());

	// Perform reparent
	BP->ParentClass = NewParentClass;

	// Refresh all nodes to pick up new parent's functions/variables
	FBlueprintEditorUtils::RefreshAllNodes(BP);

	// Compile
	FKismetEditorUtilities::CompileBlueprint(BP);

	// Save
	bool bSaved = SaveBlueprintPackage(BP);

	FString NewParentActualName = NewParentClass->GetName();

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Reparent complete, save %s"),
		bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("oldParentClass"), OldParentName);
	Result->SetStringField(TEXT("newParentClass"), NewParentActualName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// Set Blueprint Default Property Value
// ============================================================

FString FBlueprintMCPServer::HandleSetBlueprintDefault(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString PropertyName = Json->GetStringField(TEXT("property"));
	FString Value = Json->GetStringField(TEXT("value"));

	if (BlueprintName.IsEmpty() || PropertyName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, property"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	if (!BP->GeneratedClass)
	{
		return MakeErrorJson(TEXT("Blueprint has no GeneratedClass"));
	}

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	if (!CDO)
	{
		return MakeErrorJson(TEXT("Could not get Class Default Object"));
	}

	FProperty* Prop = BP->GeneratedClass->FindPropertyByName(*PropertyName);
	if (!Prop)
	{
		return MakeErrorJson(FString::Printf(TEXT("Property '%s' not found on '%s'"), *PropertyName, *BlueprintName));
	}

	FString OldValue;
	Prop->ExportTextItem_Direct(OldValue, Prop->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);

	bool bSuccess = false;
	FString ActualNewValue;

	// Handle class/soft-class properties (TSubclassOf, TSoftClassPtr)
	FClassProperty* ClassProp = CastField<FClassProperty>(Prop);
	FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop);

	if (ClassProp || SoftClassProp)
	{
		// Resolve the value to a UClass*
		UClass* ResolvedClass = nullptr;

		// Try as a C++ class name first
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == Value || It->GetName() == Value + TEXT("_C"))
			{
				ResolvedClass = *It;
				break;
			}
		}

		// Try loading as a Blueprint asset
		if (!ResolvedClass)
		{
			FString BPLoadError;
			UBlueprint* ValueBP = LoadBlueprintByName(Value, BPLoadError);
			if (ValueBP && ValueBP->GeneratedClass)
			{
				ResolvedClass = ValueBP->GeneratedClass;
			}
		}

		if (!ResolvedClass)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not resolve '%s' to a class"), *Value));
		}

		// Validate meta class compatibility
		if (ClassProp)
		{
			UClass* MetaClass = ClassProp->MetaClass;
			if (MetaClass && !ResolvedClass->IsChildOf(MetaClass))
			{
				return MakeErrorJson(FString::Printf(
					TEXT("'%s' is not a subclass of '%s' (required by property '%s')"),
					*ResolvedClass->GetName(), *MetaClass->GetName(), *PropertyName));
			}
			ClassProp->SetPropertyValue_InContainer(CDO, ResolvedClass);
		}
		else
		{
			FSoftObjectPtr SoftPtr(ResolvedClass);
			SoftClassProp->SetPropertyValue_InContainer(CDO, SoftPtr);
		}
		ActualNewValue = ResolvedClass->GetName();
		bSuccess = true;
	}
	// Handle object properties (TObjectPtr, UObject*)
	else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		// Try finding an existing object/asset by name
		UObject* ResolvedObj = nullptr;

		// Try loading as a Blueprint asset
		FString ObjLoadError;
		UBlueprint* ValueBP = LoadBlueprintByName(Value, ObjLoadError);
		if (ValueBP && ValueBP->GeneratedClass)
		{
			ResolvedObj = ValueBP->GeneratedClass->GetDefaultObject();
		}

		if (!ResolvedObj)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not resolve '%s' to an object"), *Value));
		}

		ObjProp->SetPropertyValue_InContainer(CDO, ResolvedObj);
		ActualNewValue = ResolvedObj->GetName();
		bSuccess = true;
	}
	// Handle simple types via ImportText
	else
	{
		const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, Prop->ContainerPtrToValuePtr<void>(CDO), CDO, PPF_None);
		if (ImportResult)
		{
			Prop->ExportTextItem_Direct(ActualNewValue, Prop->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
			bSuccess = true;
		}
		else
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Failed to set property '%s' to '%s' — value could not be parsed for type '%s'"),
				*PropertyName, *Value, *Prop->GetCPPType()));
		}
	}

	if (!bSuccess)
	{
		return MakeErrorJson(TEXT("Failed to set property value"));
	}

	// Mark modified and save
	CDO->MarkPackageDirty();
	BP->Modify();

	FKismetEditorUtilities::CompileBlueprint(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Set '%s.%s' from '%s' to '%s' (saved: %s)"),
		*BlueprintName, *PropertyName, *OldValue, *ActualNewValue, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("oldValue"), OldValue);
	Result->SetStringField(TEXT("newValue"), ActualNewValue);
	Result->SetStringField(TEXT("propertyType"), Prop->GetCPPType());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleCreateBlueprint — create a new Blueprint asset
// ============================================================

FString FBlueprintMCPServer::HandleCreateBlueprint(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprintName"));
	FString PackagePath = Json->GetStringField(TEXT("packagePath"));
	FString ParentClassName = Json->GetStringField(TEXT("parentClass"));
	FString BlueprintTypeStr = Json->GetStringField(TEXT("blueprintType"));

	if (BlueprintName.IsEmpty() || PackagePath.IsEmpty() || ParentClassName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprintName, packagePath, parentClass"));
	}

	// Validate packagePath starts with /Game
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return MakeErrorJson(TEXT("packagePath must start with '/Game'"));
	}

	// Check if asset already exists
	FString FullAssetPath = PackagePath / BlueprintName;
	if (FindBlueprintAsset(BlueprintName) || FindBlueprintAsset(FullAssetPath))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Blueprint '%s' already exists. Use a different name or delete the existing asset first."),
			*BlueprintName));
	}

	// Resolve parent class — try C++ class first, then Blueprint
	UClass* ParentClass = nullptr;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ParentClassName)
		{
			ParentClass = *It;
			break;
		}
	}

	if (!ParentClass)
	{
		FString ParentLoadError;
		UBlueprint* ParentBP = LoadBlueprintByName(ParentClassName, ParentLoadError);
		if (ParentBP && ParentBP->GeneratedClass)
		{
			ParentClass = ParentBP->GeneratedClass;
		}
	}

	if (!ParentClass)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Could not find parent class '%s'. Provide a C++ class name (e.g. 'Actor', 'Pawn') or Blueprint name."),
			*ParentClassName));
	}

	// Map blueprintType string to EBlueprintType
	EBlueprintType BlueprintType = BPTYPE_Normal;
	if (!BlueprintTypeStr.IsEmpty())
	{
		if (BlueprintTypeStr == TEXT("Interface"))
		{
			BlueprintType = BPTYPE_Interface;
		}
		else if (BlueprintTypeStr == TEXT("FunctionLibrary"))
		{
			BlueprintType = BPTYPE_FunctionLibrary;
		}
		else if (BlueprintTypeStr == TEXT("MacroLibrary"))
		{
			BlueprintType = BPTYPE_MacroLibrary;
		}
		else if (BlueprintTypeStr != TEXT("Normal"))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Invalid blueprintType '%s'. Valid values: Normal, Interface, FunctionLibrary, MacroLibrary"),
				*BlueprintTypeStr));
		}
	}

	// For Interface type, parent must be UInterface
	if (BlueprintType == BPTYPE_Interface && !ParentClass->IsChildOf(UInterface::StaticClass()))
	{
		// Use the engine's standard BlueprintInterface parent
		ParentClass = UInterface::StaticClass();
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating Blueprint '%s' in '%s' with parent '%s' (type=%s)"),
		*BlueprintName, *PackagePath, *ParentClass->GetName(), *BlueprintTypeStr);

	// Create the package
	FString FullPackagePath = PackagePath / BlueprintName;
	UPackage* Package = CreatePackage(*FullPackagePath);
	if (!Package)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to create package at '%s'"), *FullPackagePath));
	}

	// Create the Blueprint
	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*BlueprintName),
		BlueprintType,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass()
	);

	if (!NewBP)
	{
		return MakeErrorJson(TEXT("FKismetEditorUtilities::CreateBlueprint returned null"));
	}

	// Compile
	FKismetEditorUtilities::CompileBlueprint(NewBP);

	// Save
	bool bSaved = SaveBlueprintPackage(NewBP);

	// Refresh asset cache
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AllBlueprintAssets.Empty();
	ARM.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBlueprintAssets, true);

	// Collect graph names
	TArray<TSharedPtr<FJsonValue>> GraphNames;
	for (UEdGraph* Graph : NewBP->UbergraphPages)
	{
		GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
	}
	for (UEdGraph* Graph : NewBP->FunctionGraphs)
	{
		GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
	}
	for (UEdGraph* Graph : NewBP->MacroGraphs)
	{
		GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created Blueprint '%s' with %d graphs (saved: %s)"),
		*BlueprintName, GraphNames.Num(), bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprintName"), BlueprintName);
	Result->SetStringField(TEXT("packagePath"), PackagePath);
	Result->SetStringField(TEXT("assetPath"), FullAssetPath);
	Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());
	Result->SetStringField(TEXT("blueprintType"), BlueprintTypeStr.IsEmpty() ? TEXT("Normal") : BlueprintTypeStr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetArrayField(TEXT("graphs"), GraphNames);
	return JsonToString(Result);
}

// ============================================================
// HandleCreateGraph — create a new function, macro, or custom event graph
// ============================================================

FString FBlueprintMCPServer::HandleCreateGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString GraphName = Json->GetStringField(TEXT("graphName"));
	FString GraphType = Json->GetStringField(TEXT("graphType"));

	if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || GraphType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, graphName, graphType"));
	}

	if (GraphType != TEXT("function") && GraphType != TEXT("macro") && GraphType != TEXT("customEvent"))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Invalid graphType '%s'. Valid values: function, macro, customEvent"), *GraphType));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Check graph name uniqueness
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Existing : AllGraphs)
	{
		if (Existing && Existing->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("A graph named '%s' already exists in Blueprint '%s'"), *GraphName, *BlueprintName));
		}
	}

	// Also check for existing custom events with the same name
	if (GraphType == TEXT("customEvent"))
	{
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CE->CustomFunctionName == FName(*GraphName))
					{
						return MakeErrorJson(FString::Printf(
							TEXT("A custom event named '%s' already exists in Blueprint '%s'"), *GraphName, *BlueprintName));
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating %s graph '%s' in Blueprint '%s'"),
		*GraphType, *GraphName, *BlueprintName);

	FString CreatedNodeId;

	if (GraphType == TEXT("function"))
	{
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*GraphName),
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			return MakeErrorJson(TEXT("Failed to create function graph"));
		}
		FBlueprintEditorUtils::AddFunctionGraph(BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/static_cast<UClass*>(nullptr));
	}
	else if (GraphType == TEXT("macro"))
	{
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*GraphName),
			UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!NewGraph)
		{
			return MakeErrorJson(TEXT("Failed to create macro graph"));
		}
		FBlueprintEditorUtils::AddMacroGraph(BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);
	}
	else // customEvent
	{
		// Find the EventGraph (first UbergraphPage)
		UEdGraph* EventGraph = nullptr;
		if (BP->UbergraphPages.Num() > 0)
		{
			EventGraph = BP->UbergraphPages[0];
		}
		if (!EventGraph)
		{
			return MakeErrorJson(TEXT("Blueprint has no EventGraph to add a custom event to"));
		}

		// Create a custom event node in the EventGraph
		UK2Node_CustomEvent* NewEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
		NewEvent->CustomFunctionName = FName(*GraphName);
		NewEvent->bIsEditable = true;
		EventGraph->AddNode(NewEvent, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		NewEvent->CreateNewGuid();
		NewEvent->PostPlacedNewNode();
		NewEvent->AllocateDefaultPins();
		CreatedNodeId = NewEvent->NodeGuid.ToString();
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created %s graph '%s' in '%s' (saved: %s)"),
		*GraphType, *GraphName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("graphName"), GraphName);
	Result->SetStringField(TEXT("graphType"), GraphType);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!CreatedNodeId.IsEmpty())
	{
		Result->SetStringField(TEXT("nodeId"), CreatedNodeId);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleAddVariable — add a new member variable to a Blueprint
// ============================================================

FString FBlueprintMCPServer::HandleAddVariable(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString VariableName = Json->GetStringField(TEXT("variableName"));
	FString VariableType = Json->GetStringField(TEXT("variableType"));

	if (BlueprintName.IsEmpty() || VariableName.IsEmpty() || VariableType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, variableName, variableType"));
	}

	FString Category;
	if (Json->HasField(TEXT("category")))
	{
		Category = Json->GetStringField(TEXT("category"));
	}

	bool bIsArray = false;
	if (Json->HasField(TEXT("isArray")))
	{
		bIsArray = Json->GetBoolField(TEXT("isArray"));
	}

	FString DefaultValue;
	if (Json->HasField(TEXT("defaultValue")))
	{
		DefaultValue = Json->GetStringField(TEXT("defaultValue"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Check for duplicate variable name
	FName VarFName(*VariableName);
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Variable '%s' already exists in Blueprint '%s'"), *VariableName, *BlueprintName));
		}
	}

	// Build FEdGraphPinType based on variableType
	FEdGraphPinType PinType;
	FString TypeLower = VariableType.ToLower();

	if (TypeLower == TEXT("bool") || TypeLower == TEXT("boolean"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (TypeLower == TEXT("int") || TypeLower == TEXT("int32") || TypeLower == TEXT("integer"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (TypeLower == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (TypeLower == TEXT("float") || TypeLower == TEXT("double") || TypeLower == TEXT("real"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("double");
	}
	else if (TypeLower == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (TypeLower == TEXT("name"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (TypeLower == TEXT("text"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (TypeLower == TEXT("byte"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	}
	else if (TypeLower == TEXT("vector") || TypeLower == TEXT("fvector"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (TypeLower == TEXT("rotator") || TypeLower == TEXT("frotator"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (TypeLower == TEXT("transform") || TypeLower == TEXT("ftransform"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (TypeLower == TEXT("linearcolor") || TypeLower == TEXT("flinearcolor"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (TypeLower == TEXT("vector2d") || TypeLower == TEXT("fvector2d"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		PinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
	}
	else if (TypeLower == TEXT("object"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategoryObject = UObject::StaticClass();
	}
	else
	{
		// Try as a struct (F-prefix or raw name)
		FString InternalName = VariableType;
		bool bTriedAsStruct = false;

		if (VariableType.StartsWith(TEXT("F")) || VariableType.StartsWith(TEXT("S_")) || (!VariableType.StartsWith(TEXT("E"))))
		{
			if (VariableType.StartsWith(TEXT("F")))
			{
				InternalName = VariableType.Mid(1);
			}

			UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*InternalName);
			if (!FoundStruct)
			{
				for (TObjectIterator<UScriptStruct> It; It; ++It)
				{
					if (It->GetName() == InternalName || It->GetName() == VariableType)
					{
						FoundStruct = *It;
						break;
					}
				}
			}

			if (FoundStruct)
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FoundStruct;
				bTriedAsStruct = true;
			}
		}

		if (!bTriedAsStruct)
		{
			// Try as an enum (E-prefix or raw name)
			FString EnumInternalName = VariableType;
			if (VariableType.StartsWith(TEXT("E")))
			{
				EnumInternalName = VariableType.Mid(1);
			}

			UEnum* FoundEnum = FindFirstObject<UEnum>(*EnumInternalName);
			if (!FoundEnum)
			{
				for (TObjectIterator<UEnum> It; It; ++It)
				{
					if (It->GetName() == EnumInternalName || It->GetName() == VariableType)
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
					PinType.PinCategory = UEdGraphSchema_K2::PC_Enum;
				}
				else
				{
					PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				}
				PinType.PinSubCategoryObject = FoundEnum;
			}
			else
			{
				return MakeErrorJson(FString::Printf(
					TEXT("Unknown variable type '%s'. Use: bool, int, float, string, name, text, byte, vector, rotator, transform, or a struct/enum name (e.g. FVector, EMyEnum)"),
					*VariableType));
			}
		}
	}

	// Set container type for arrays
	if (bIsArray)
	{
		PinType.ContainerType = EPinContainerType::Array;
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding variable '%s' (type=%s, array=%s) to Blueprint '%s'"),
		*VariableName, *VariableType, bIsArray ? TEXT("true") : TEXT("false"), *BlueprintName);

	// Add the variable using the editor utility function
	bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(BP, VarFName, PinType, DefaultValue);
	if (!bSuccess)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("FBlueprintEditorUtils::AddMemberVariable failed for '%s'"), *VariableName));
	}

	// Set category if provided
	if (!Category.IsEmpty())
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(BP, VarFName, nullptr, FText::FromString(Category));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added variable '%s' to '%s' (saved: %s)"),
		*VariableName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("variableName"), VariableName);
	Result->SetStringField(TEXT("variableType"), VariableType);
	if (!Category.IsEmpty())
	{
		Result->SetStringField(TEXT("category"), Category);
	}
	Result->SetBoolField(TEXT("isArray"), bIsArray);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveVariable — remove a member variable from a Blueprint
// ============================================================

FString FBlueprintMCPServer::HandleRemoveVariable(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString VariableName = Json->GetStringField(TEXT("variableName"));

	if (BlueprintName.IsEmpty() || VariableName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, variableName"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find variable by name (case-insensitive)
	FName VarFName(*VariableName);
	bool bVarFound = false;
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName.ToString().Equals(VariableName, ESearchCase::IgnoreCase))
		{
			VarFName = Var.VarName; // Use the exact name found
			bVarFound = true;
			break;
		}
	}

	if (!bVarFound)
	{
		// Build available variables list for helpful error message
		TArray<TSharedPtr<FJsonValue>> AvailVars;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			AvailVars.Add(MakeShared<FJsonValueString>(Var.VarName.ToString()));
		}

		TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *BlueprintName));
		ErrorResult->SetArrayField(TEXT("availableVariables"), AvailVars);
		return JsonToString(ErrorResult);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removing variable '%s' from Blueprint '%s'"),
		*VariableName, *BlueprintName);

	// Use the editor utility to remove the variable (also cleans up Get/Set nodes)
	FBlueprintEditorUtils::RemoveMemberVariable(BP, VarFName);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed variable '%s' from '%s' (saved: %s)"),
		*VariableName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("variableName"), VariableName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
