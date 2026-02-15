#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

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
// HandleDeleteGraph — delete a function or macro graph
// ============================================================

FString FBlueprintMCPServer::HandleDeleteGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body"));

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString GraphName = Json->GetStringField(TEXT("graphName"));

	if (BlueprintName.IsEmpty() || GraphName.IsEmpty())
		return MakeErrorJson(TEXT("Missing required fields: blueprint, graphName"));

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP) return MakeErrorJson(LoadError);

	// Find the graph
	UEdGraph* TargetGraph = nullptr;
	FString GraphType;

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			TargetGraph = Graph;
			GraphType = TEXT("function");
			break;
		}
	}
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->MacroGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				GraphType = TEXT("macro");
				break;
			}
		}
	}

	// Check if it's an UbergraphPage (EventGraph) — disallow deletion
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return MakeErrorJson(FString::Printf(
					TEXT("Cannot delete UbergraphPage '%s'. EventGraph and other Ubergraph pages cannot be deleted."),
					*GraphName));
			}
		}
		return MakeErrorJson(FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deleting %s graph '%s' from Blueprint '%s'"),
		*GraphType, *GraphName, *BlueprintName);

	// Count nodes for reporting
	int32 NodeCount = TargetGraph->Nodes.Num();

	// Remove the graph
	FBlueprintEditorUtils::RemoveGraph(BP, TargetGraph, EGraphRemoveFlags::Default);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deleted graph '%s' (%d nodes), save %s"),
		*GraphName, NodeCount, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("graphName"), GraphName);
	Result->SetStringField(TEXT("graphType"), GraphType);
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRenameGraph — rename a function or macro graph
// ============================================================

FString FBlueprintMCPServer::HandleRenameGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body"));

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString GraphName = Json->GetStringField(TEXT("graphName"));
	FString NewName = Json->GetStringField(TEXT("newName"));

	if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || NewName.IsEmpty())
		return MakeErrorJson(TEXT("Missing required fields: blueprint, graphName, newName"));

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP) return MakeErrorJson(LoadError);

	// Check if it's an UbergraphPage — disallow rename
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Cannot rename UbergraphPage '%s'. EventGraph and other Ubergraph pages cannot be renamed."),
				*GraphName));
		}
	}

	// Find the graph in FunctionGraphs or MacroGraphs
	UEdGraph* TargetGraph = nullptr;
	FString GraphType;

	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
		{
			TargetGraph = Graph;
			GraphType = TEXT("function");
			break;
		}
	}
	if (!TargetGraph)
	{
		for (UEdGraph* Graph : BP->MacroGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				GraphType = TEXT("macro");
				break;
			}
		}
	}

	if (!TargetGraph)
		return MakeErrorJson(FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintName));

	// Check for name collision
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Existing : AllGraphs)
	{
		if (Existing && Existing != TargetGraph && Existing->GetName().Equals(NewName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("A graph named '%s' already exists in Blueprint '%s'"), *NewName, *BlueprintName));
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Renaming %s graph '%s' to '%s' in Blueprint '%s'"),
		*GraphType, *GraphName, *NewName, *BlueprintName);

	FBlueprintEditorUtils::RenameGraph(TargetGraph, NewName);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Renamed graph '%s' to '%s', save %s"),
		*GraphName, *NewName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("oldName"), GraphName);
	Result->SetStringField(TEXT("newName"), TargetGraph->GetName());
	Result->SetStringField(TEXT("graphType"), GraphType);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
