#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

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

	// Reconstruct the node to update output pins with the new type (use schema for MinimalAPI compat)
	if (UEdGraph* OwningGraph = EntryNode->GetGraph())
	{
		if (const UEdGraphSchema* Schema = OwningGraph->GetSchema())
		{
			Schema->ReconstructNode(*EntryNode);
		}
	}

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

	// Reconstruct the node to update output pins (use schema for MinimalAPI compat)
	if (UEdGraph* OwningGraph = EntryNode->GetGraph())
	{
		if (const UEdGraphSchema* Schema = OwningGraph->GetSchema())
		{
			Schema->ReconstructNode(*EntryNode);
		}
	}

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
// HandleAddFunctionParameter
// ============================================================

FString FBlueprintMCPServer::HandleAddFunctionParameter(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString FunctionName = Json->GetStringField(TEXT("functionName"));
	FString ParamName = Json->GetStringField(TEXT("paramName"));
	FString ParamType = Json->GetStringField(TEXT("paramType"));

	if (BlueprintName.IsEmpty() || FunctionName.IsEmpty() || ParamName.IsEmpty() || ParamType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, functionName, paramName, paramType"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Resolve param type
	FEdGraphPinType PinType;
	FString TypeError;
	if (!ResolveTypeFromString(ParamType, PinType, TypeError))
	{
		return MakeErrorJson(TypeError);
	}

	// Find the entry node using 3 strategies
	UK2Node_EditablePinBase* EntryNode = nullptr;
	FString NodeType;

	FName FuncFName(*FunctionName);

	// Strategy 1: K2Node_FunctionEntry in function graphs
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph || !Graph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
		{
			continue;
		}

		// Skip delegate signature graphs (handled in Strategy 3)
		if (BP->DelegateSignatureGraphs.Contains(Graph))
		{
			continue;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
			{
				EntryNode = FE;
				NodeType = TEXT("FunctionEntry");
				break;
			}
		}
		if (EntryNode) break;
	}

	// Strategy 2: K2Node_CustomEvent with matching CustomFunctionName
	if (!EntryNode)
	{
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					if (CE->CustomFunctionName.ToString().Equals(FunctionName, ESearchCase::IgnoreCase))
					{
						EntryNode = CE;
						NodeType = TEXT("CustomEvent");
						break;
					}
				}
			}
			if (EntryNode) break;
		}
	}

	// Strategy 3: K2Node_FunctionEntry in DelegateSignatureGraphs
	if (!EntryNode)
	{
		for (UEdGraph* SigGraph : BP->DelegateSignatureGraphs)
		{
			if (!SigGraph || !SigGraph->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				continue;
			}

			for (UEdGraphNode* Node : SigGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
				{
					EntryNode = FE;
					NodeType = TEXT("EventDispatcher");
					break;
				}
			}
			if (EntryNode) break;
		}
	}

	if (!EntryNode)
	{
		// Build a helpful error listing available functions, events, and dispatchers
		TArray<TSharedPtr<FJsonValue>> AvailFuncs;

		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph) AvailFuncs.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}

		// Custom events
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					AvailFuncs.Add(MakeShared<FJsonValueString>(
						FString::Printf(TEXT("%s (custom event)"), *CE->CustomFunctionName.ToString())));
				}
			}
		}

		// Dispatchers
		TSet<FName> DelegateNames;
		FBlueprintEditorUtils::GetDelegateNameList(BP, DelegateNames);
		for (const FName& DN : DelegateNames)
		{
			AvailFuncs.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s (event dispatcher)"), *DN.ToString())));
		}

		TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Function, custom event, or event dispatcher '%s' not found in Blueprint '%s'"),
			*FunctionName, *BlueprintName));
		ErrorResult->SetArrayField(TEXT("availableFunctions"), AvailFuncs);
		return JsonToString(ErrorResult);
	}

	// Check for duplicate parameter name
	for (const TSharedPtr<FUserPinInfo>& Existing : EntryNode->UserDefinedPins)
	{
		if (Existing.IsValid() && Existing->PinName.ToString().Equals(ParamName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Parameter '%s' already exists on '%s'"), *ParamName, *FunctionName));
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding parameter '%s' (type=%s) to %s '%s' in Blueprint '%s'"),
		*ParamName, *ParamType, *NodeType, *FunctionName, *BlueprintName);

	// Add the parameter pin (EGPD_Output on entry = input to callers)
	EntryNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added parameter '%s' to '%s' in '%s' (saved: %s)"),
		*ParamName, *FunctionName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("functionName"), FunctionName);
	Result->SetStringField(TEXT("paramName"), ParamName);
	Result->SetStringField(TEXT("paramType"), ParamType);
	Result->SetStringField(TEXT("nodeType"), NodeType);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
