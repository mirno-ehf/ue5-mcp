#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
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
#include "K2Node_DynamicCast.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Select.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

// SEH wrapper defined in BlueprintMCPServer.cpp — non-static for cross-TU access
#if PLATFORM_WINDOWS
extern int32 TryRefreshAllNodesSEH(UBlueprint* BP);
#endif

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

	// Check for batch mode
	const TArray<TSharedPtr<FJsonValue>>* BatchArray = nullptr;
	if (Json->TryGetArrayField(TEXT("batch"), BatchArray) && BatchArray && BatchArray->Num() > 0)
	{
		// Batch mode: process multiple pin default operations
		TArray<TSharedPtr<FJsonValue>> Results;
		int32 SuccessCount = 0;
		TSet<UBlueprint*> ModifiedBlueprints;

		for (const TSharedPtr<FJsonValue>& OpVal : *BatchArray)
		{
			TSharedPtr<FJsonObject> OpObj = OpVal->AsObject();
			if (!OpObj.IsValid())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("error"), TEXT("Invalid batch entry"));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			FString OpBlueprint = OpObj->GetStringField(TEXT("blueprint"));
			FString OpNodeId = OpObj->GetStringField(TEXT("nodeId"));
			FString OpPinName = OpObj->GetStringField(TEXT("pinName"));
			FString OpValue = OpObj->GetStringField(TEXT("value"));

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("blueprint"), OpBlueprint);
			Entry->SetStringField(TEXT("nodeId"), OpNodeId);
			Entry->SetStringField(TEXT("pinName"), OpPinName);

			if (OpBlueprint.IsEmpty() || OpNodeId.IsEmpty() || OpPinName.IsEmpty())
			{
				Entry->SetStringField(TEXT("error"), TEXT("Missing required fields: blueprint, nodeId, pinName"));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			FString LoadError;
			UBlueprint* BP = LoadBlueprintByName(OpBlueprint, LoadError);
			if (!BP)
			{
				Entry->SetStringField(TEXT("error"), LoadError);
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			UEdGraph* Graph = nullptr;
			UEdGraphNode* Node = FindNodeByGuid(BP, OpNodeId, &Graph);
			if (!Node)
			{
				Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Node '%s' not found"), *OpNodeId));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			UEdGraphPin* Pin = Node->FindPin(FName(*OpPinName));
			if (!Pin)
			{
				Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *OpPinName, *OpNodeId));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			if (Pin->Direction != EGPD_Input)
			{
				Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin '%s' is an output pin"), *OpPinName));
				Results.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}

			FString OldValue = Pin->DefaultValue;

			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (Schema)
			{
				Schema->TrySetDefaultValue(*Pin, OpValue);
			}
			else
			{
				Pin->DefaultValue = OpValue;
			}

			Entry->SetBoolField(TEXT("success"), true);
			Entry->SetStringField(TEXT("oldValue"), OldValue);
			Entry->SetStringField(TEXT("newValue"), Pin->DefaultValue);
			Results.Add(MakeShared<FJsonValueObject>(Entry));
			SuccessCount++;
			ModifiedBlueprints.Add(BP);
		}

		// Save all modified blueprints
		bool bAllSaved = true;
		for (UBlueprint* BP : ModifiedBlueprints)
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
			FKismetEditorUtilities::CompileBlueprint(BP);
			if (!SaveBlueprintPackage(BP))
			{
				bAllSaved = false;
			}
		}

		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Batch SetPinDefault — %d/%d succeeded, save %s"),
			SuccessCount, BatchArray->Num(), bAllSaved ? TEXT("true") : TEXT("false"));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("successCount"), SuccessCount);
		Result->SetNumberField(TEXT("totalCount"), BatchArray->Num());
		Result->SetArrayField(TEXT("results"), Results);
		Result->SetBoolField(TEXT("saved"), bAllSaved);
		return JsonToString(Result);
	}

	// Single-pin mode (existing logic)
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
	else if (NodeType == TEXT("Branch"))
	{
		UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(TargetGraph);
		BranchNode->NodePosX = PosX;
		BranchNode->NodePosY = PosY;
		TargetGraph->AddNode(BranchNode, false, false);
		BranchNode->AllocateDefaultPins();
		NewNode = BranchNode;
	}
	else if (NodeType == TEXT("Sequence"))
	{
		UK2Node_ExecutionSequence* SeqNode = NewObject<UK2Node_ExecutionSequence>(TargetGraph);
		SeqNode->NodePosX = PosX;
		SeqNode->NodePosY = PosY;
		TargetGraph->AddNode(SeqNode, false, false);
		SeqNode->AllocateDefaultPins();
		NewNode = SeqNode;
	}
	else if (NodeType == TEXT("CustomEvent"))
	{
		FString EventName = Json->GetStringField(TEXT("eventName"));
		if (EventName.IsEmpty())
		{
			return MakeErrorJson(TEXT("Missing required field 'eventName' for CustomEvent"));
		}

		UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(TargetGraph);
		EventNode->CustomFunctionName = FName(*EventName);
		EventNode->NodePosX = PosX;
		EventNode->NodePosY = PosY;
		TargetGraph->AddNode(EventNode, false, false);
		EventNode->AllocateDefaultPins();
		NewNode = EventNode;
	}
	else if (NodeType == TEXT("ForEachLoop") || NodeType == TEXT("ForLoop") || NodeType == TEXT("ForLoopWithBreak") || NodeType == TEXT("WhileLoop"))
	{
		// These are all macro instances from the engine standard macro library
		FString MacroName;
		if (NodeType == TEXT("ForEachLoop")) MacroName = TEXT("ForEachLoop");
		else if (NodeType == TEXT("ForLoop")) MacroName = TEXT("ForLoop");
		else if (NodeType == TEXT("ForLoopWithBreak")) MacroName = TEXT("ForLoopWithBreak");
		else MacroName = TEXT("WhileLoop");

		// Load the standard macros Blueprint from the engine
		UBlueprint* StandardMacros = Cast<UBlueprint>(StaticLoadObject(
			UBlueprint::StaticClass(), nullptr,
			TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")));

		UEdGraph* MacroGraph = nullptr;
		if (StandardMacros)
		{
			for (UEdGraph* Graph : StandardMacros->MacroGraphs)
			{
				if (Graph && Graph->GetName() == MacroName)
				{
					MacroGraph = Graph;
					break;
				}
			}
		}

		if (!MacroGraph)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Standard macro '%s' not found. Ensure the engine standard macros are loaded."), *MacroName));
		}

		UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(TargetGraph);
		MacroNode->SetMacroGraph(MacroGraph);
		MacroNode->NodePosX = PosX;
		MacroNode->NodePosY = PosY;
		TargetGraph->AddNode(MacroNode, false, false);
		MacroNode->AllocateDefaultPins();
		NewNode = MacroNode;
	}
	else if (NodeType == TEXT("SpawnActorFromClass"))
	{
		FString ClassName = Json->GetStringField(TEXT("actorClass"));
		// actorClass is optional — if not provided, user can set it via the class pin later

		UClass* ActorClass = nullptr;
		if (!ClassName.IsEmpty())
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if ((It->GetName() == ClassName || It->GetName() == ClassName + TEXT("_C")) && It->IsChildOf(AActor::StaticClass()))
				{
					ActorClass = *It;
					break;
				}
			}
			if (!ActorClass)
			{
				return MakeErrorJson(FString::Printf(TEXT("Actor class '%s' not found"), *ClassName));
			}
		}

		UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(TargetGraph);
		SpawnNode->NodePosX = PosX;
		SpawnNode->NodePosY = PosY;
		TargetGraph->AddNode(SpawnNode, false, false);
		SpawnNode->AllocateDefaultPins();
		if (ActorClass)
		{
			UEdGraphPin* ClassPin = SpawnNode->GetClassPin();
			if (ClassPin)
			{
				ClassPin->DefaultObject = ActorClass;
				SpawnNode->ReconstructNode();
			}
		}
		NewNode = SpawnNode;
	}
	else if (NodeType == TEXT("Select"))
	{
		UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(TargetGraph);
		SelectNode->NodePosX = PosX;
		SelectNode->NodePosY = PosY;
		TargetGraph->AddNode(SelectNode, false, false);
		SelectNode->AllocateDefaultPins();
		NewNode = SelectNode;
	}
	else
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Unsupported nodeType '%s'. Supported: BreakStruct, MakeStruct, CallFunction, VariableGet, VariableSet, DynamicCast, OverrideEvent, CallParentFunction, Branch, Sequence, CustomEvent, ForEachLoop, ForLoop, ForLoopWithBreak, WhileLoop, SpawnActorFromClass, Select"),
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
// HandleMoveNode — reposition one or more nodes in a blueprint graph
// ============================================================

FString FBlueprintMCPServer::HandleMoveNode(const FString& Body)
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

	// Check for batch mode
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	bool bBatchMode = Json->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray && NodesArray->Num() > 0;

	if (bBatchMode)
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		int32 SuccessCount = 0;

		for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
		{
			TSharedPtr<FJsonObject> NodeObj = NodeVal->AsObject();
			if (!NodeObj.IsValid()) continue;

			FString NodeId = NodeObj->GetStringField(TEXT("nodeId"));
			int32 X = (int32)NodeObj->GetNumberField(TEXT("x"));
			int32 Y = (int32)NodeObj->GetNumberField(TEXT("y"));

			TSharedRef<FJsonObject> EntryResult = MakeShared<FJsonObject>();
			EntryResult->SetStringField(TEXT("nodeId"), NodeId);

			UEdGraphNode* Node = FindNodeByGuid(BP, NodeId);
			if (!Node)
			{
				EntryResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Node '%s' not found"), *NodeId));
				Results.Add(MakeShared<FJsonValueObject>(EntryResult));
				continue;
			}

			int32 OldX = Node->NodePosX;
			int32 OldY = Node->NodePosY;
			Node->NodePosX = X;
			Node->NodePosY = Y;
			EntryResult->SetBoolField(TEXT("success"), true);
			EntryResult->SetNumberField(TEXT("oldX"), OldX);
			EntryResult->SetNumberField(TEXT("oldY"), OldY);
			EntryResult->SetNumberField(TEXT("newX"), Node->NodePosX);
			EntryResult->SetNumberField(TEXT("newY"), Node->NodePosY);
			Results.Add(MakeShared<FJsonValueObject>(EntryResult));
			SuccessCount++;
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		bool bSaved = SaveBlueprintPackage(BP);

		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: MoveNode batch — %d/%d succeeded, save %s"),
			SuccessCount, NodesArray->Num(), bSaved ? TEXT("true") : TEXT("false"));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetNumberField(TEXT("movedCount"), SuccessCount);
		Result->SetNumberField(TEXT("totalRequested"), NodesArray->Num());
		Result->SetArrayField(TEXT("results"), Results);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return JsonToString(Result);
	}

	// Single node mode
	FString NodeId = Json->GetStringField(TEXT("nodeId"));
	if (NodeId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: nodeId (or use 'nodes' array for batch mode)"));
	}

	if (!Json->HasField(TEXT("x")) || !Json->HasField(TEXT("y")))
	{
		return MakeErrorJson(TEXT("Missing required fields: x, y"));
	}

	int32 X = (int32)Json->GetNumberField(TEXT("x"));
	int32 Y = (int32)Json->GetNumberField(TEXT("y"));

	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	int32 OldX = Node->NodePosX;
	int32 OldY = Node->NodePosY;
	Node->NodePosX = X;
	Node->NodePosY = Y;

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: MoveNode '%s' from (%d,%d) to (%d,%d)"),
		*NodeId, OldX, OldY, X, Y);

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetNumberField(TEXT("oldX"), OldX);
	Result->SetNumberField(TEXT("oldY"), OldY);
	Result->SetNumberField(TEXT("newX"), Node->NodePosX);
	Result->SetNumberField(TEXT("newY"), Node->NodePosY);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleDuplicateNodes — duplicate one or more nodes in a graph
// ============================================================

FString FBlueprintMCPServer::HandleDuplicateNodes(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString GraphName = Json->GetStringField(TEXT("graph"));

	if (BlueprintName.IsEmpty() || GraphName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, graph"));
	}

	// Get node IDs
	const TArray<TSharedPtr<FJsonValue>>* NodeIdsArray = nullptr;
	if (!Json->TryGetArrayField(TEXT("nodeIds"), NodeIdsArray) || !NodeIdsArray || NodeIdsArray->Num() == 0)
	{
		return MakeErrorJson(TEXT("Missing required field: nodeIds (array of node GUIDs)"));
	}

	int32 OffsetX = 50;
	int32 OffsetY = 50;
	if (Json->HasField(TEXT("offsetX")))
		OffsetX = (int32)Json->GetNumberField(TEXT("offsetX"));
	if (Json->HasField(TEXT("offsetY")))
		OffsetY = (int32)Json->GetNumberField(TEXT("offsetY"));

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the target graph
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
		return MakeErrorJson(FString::Printf(TEXT("Graph '%s' not found"), *DecodedGraphName));
	}

	// Find all source nodes
	TArray<UEdGraphNode*> SourceNodes;
	TArray<FString> NotFound;

	for (const TSharedPtr<FJsonValue>& IdVal : *NodeIdsArray)
	{
		FString NodeId = IdVal->AsString();
		UEdGraphNode* Node = FindNodeByGuid(BP, NodeId);
		if (Node)
		{
			// Verify it's in the target graph
			if (Node->GetGraph() == TargetGraph)
			{
				SourceNodes.Add(Node);
			}
			else
			{
				NotFound.Add(FString::Printf(TEXT("%s (in different graph)"), *NodeId));
			}
		}
		else
		{
			NotFound.Add(NodeId);
		}
	}

	if (SourceNodes.Num() == 0)
	{
		return MakeErrorJson(TEXT("No valid nodes found to duplicate"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Duplicating %d node(s) in graph '%s' of '%s'"),
		SourceNodes.Num(), *DecodedGraphName, *BlueprintName);

	// Duplicate each node
	TArray<TSharedPtr<FJsonValue>> DuplicatedNodes;
	TMap<FGuid, FGuid> OldToNewGuidMap;

	for (UEdGraphNode* SourceNode : SourceNodes)
	{
		// Duplicate the node using DuplicateObject
		UEdGraphNode* NewNode = DuplicateObject<UEdGraphNode>(SourceNode, TargetGraph);
		if (!NewNode)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("sourceNodeId"), SourceNode->NodeGuid.ToString());
			Entry->SetStringField(TEXT("error"), TEXT("DuplicateObject failed"));
			DuplicatedNodes.Add(MakeShared<FJsonValueObject>(Entry));
			continue;
		}

		// Assign new GUID
		NewNode->CreateNewGuid();
		OldToNewGuidMap.Add(SourceNode->NodeGuid, NewNode->NodeGuid);

		// Offset position
		NewNode->NodePosX += OffsetX;
		NewNode->NodePosY += OffsetY;

		// Break all connections on the duplicate (they point to old pin instances)
		for (UEdGraphPin* Pin : NewNode->Pins)
		{
			if (Pin)
			{
				Pin->LinkedTo.Empty();
			}
		}

		// Add to graph
		TargetGraph->AddNode(NewNode, false, false);

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("sourceNodeId"), SourceNode->NodeGuid.ToString());
		Entry->SetStringField(TEXT("newNodeId"), NewNode->NodeGuid.ToString());
		Entry->SetNumberField(TEXT("posX"), NewNode->NodePosX);
		Entry->SetNumberField(TEXT("posY"), NewNode->NodePosY);
		Entry->SetStringField(TEXT("nodeClass"), NewNode->GetClass()->GetName());
		Entry->SetStringField(TEXT("nodeTitle"), NewNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		DuplicatedNodes.Add(MakeShared<FJsonValueObject>(Entry));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Duplicated %d node(s), save %s"),
		DuplicatedNodes.Num(), bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("graph"), DecodedGraphName);
	Result->SetNumberField(TEXT("duplicatedCount"), DuplicatedNodes.Num());
	Result->SetArrayField(TEXT("nodes"), DuplicatedNodes);
	Result->SetBoolField(TEXT("saved"), bSaved);

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& NF : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(NF));
		}
		Result->SetArrayField(TEXT("notFound"), NotFoundArr);
	}

	return JsonToString(Result);
}
