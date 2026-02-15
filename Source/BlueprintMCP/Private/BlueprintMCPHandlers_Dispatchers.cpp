#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleAddEventDispatcher — create a multicast delegate on a Blueprint
// ============================================================

FString FBlueprintMCPServer::HandleAddEventDispatcher(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString DispatcherName = Json->GetStringField(TEXT("dispatcherName"));

	if (BlueprintName.IsEmpty() || DispatcherName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, dispatcherName"));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	FName DispatcherFName(*DispatcherName);

	// Check for name uniqueness against existing variables
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == DispatcherFName)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("A variable or dispatcher named '%s' already exists in Blueprint '%s'"),
				*DispatcherName, *BlueprintName));
		}
	}

	// Check against existing graphs (functions, macros, etc.)
	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);
	for (UEdGraph* Existing : AllGraphs)
	{
		if (Existing && Existing->GetName().Equals(DispatcherName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("A graph named '%s' already exists in Blueprint '%s'"),
				*DispatcherName, *BlueprintName));
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding event dispatcher '%s' to Blueprint '%s'"),
		*DispatcherName, *BlueprintName);

	// Step 1: Add a member variable with PC_MCDelegate pin type
	FEdGraphPinType DelegateType;
	DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
	bool bVarAdded = FBlueprintEditorUtils::AddMemberVariable(BP, DispatcherFName, DelegateType);
	if (!bVarAdded)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to add delegate variable for '%s'"), *DispatcherName));
	}

	// Step 2: Create the signature graph
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(BP, DispatcherFName,
		UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!SigGraph)
	{
		return MakeErrorJson(TEXT("Failed to create delegate signature graph"));
	}

	K2Schema->CreateDefaultNodesForGraph(*SigGraph);
	K2Schema->CreateFunctionGraphTerminators(*SigGraph, static_cast<UClass*>(nullptr));
	K2Schema->AddExtraFunctionFlags(SigGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
	K2Schema->MarkFunctionEntryAsEditable(SigGraph, true);

	BP->DelegateSignatureGraphs.Add(SigGraph);

	// Step 3: Add parameters if provided
	TArray<TSharedPtr<FJsonValue>> ParamsArr;
	if (Json->HasField(TEXT("parameters")))
	{
		ParamsArr = Json->GetArrayField(TEXT("parameters"));
	}

	TArray<TSharedPtr<FJsonValue>> AddedParamsJson;

	if (ParamsArr.Num() > 0)
	{
		// Find the entry node in the signature graph
		UK2Node_EditablePinBase* EntryNode = nullptr;
		for (UEdGraphNode* Node : SigGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node))
			{
				EntryNode = FE;
				break;
			}
		}

		if (!EntryNode)
		{
			// Still save what we have
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			SaveBlueprintPackage(BP);
			return MakeErrorJson(TEXT("Event dispatcher created but entry node not found — parameters could not be added"));
		}

		for (const TSharedPtr<FJsonValue>& ParamVal : ParamsArr)
		{
			if (!ParamVal.IsValid() || ParamVal->Type != EJson::Object) continue;
			TSharedPtr<FJsonObject> ParamObj = ParamVal->AsObject();

			FString ParamName = ParamObj->GetStringField(TEXT("name"));
			FString ParamType = ParamObj->GetStringField(TEXT("type"));

			if (ParamName.IsEmpty() || ParamType.IsEmpty()) continue;

			FEdGraphPinType PinType;
			FString TypeError;
			if (!ResolveTypeFromString(ParamType, PinType, TypeError))
			{
				return MakeErrorJson(FString::Printf(
					TEXT("Parameter '%s': %s"), *ParamName, *TypeError));
			}

			EntryNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);

			TSharedRef<FJsonObject> ParamJson = MakeShared<FJsonObject>();
			ParamJson->SetStringField(TEXT("name"), ParamName);
			ParamJson->SetStringField(TEXT("type"), ParamType);
			AddedParamsJson.Add(MakeShared<FJsonValueObject>(ParamJson));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added event dispatcher '%s' to '%s' with %d params (saved: %s)"),
		*DispatcherName, *BlueprintName, AddedParamsJson.Num(), bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("dispatcherName"), DispatcherName);
	Result->SetArrayField(TEXT("parameters"), AddedParamsJson);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleListEventDispatchers — list all event dispatchers on a Blueprint
// ============================================================

FString FBlueprintMCPServer::HandleListEventDispatchers(const FString& Body)
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

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	TSet<FName> DelegateNameSet;
	FBlueprintEditorUtils::GetDelegateNameList(BP, DelegateNameSet);

	TArray<TSharedPtr<FJsonValue>> DispatchersArr;

	for (const FName& DelegateName : DelegateNameSet)
	{
		TSharedRef<FJsonObject> DispObj = MakeShared<FJsonObject>();
		DispObj->SetStringField(TEXT("name"), DelegateName.ToString());

		// Get parameter info from the signature graph
		TArray<TSharedPtr<FJsonValue>> ParamsArr;

		UEdGraph* SigGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(BP, DelegateName);
		if (SigGraph)
		{
			for (UEdGraphNode* Node : SigGraph->Nodes)
			{
				UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node);
				if (!FE) continue;

				for (const TSharedPtr<FUserPinInfo>& PinInfo : FE->UserDefinedPins)
				{
					if (!PinInfo.IsValid()) continue;

					TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), PinInfo->PinName.ToString());

					// Build a human-readable type name from the pin type
					FString TypeStr = PinInfo->PinType.PinCategory.ToString();
					if (PinInfo->PinType.PinSubCategoryObject.IsValid())
					{
						TypeStr = PinInfo->PinType.PinSubCategoryObject->GetName();
					}
					ParamObj->SetStringField(TEXT("type"), TypeStr);

					ParamsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
				break; // only need the first entry node
			}
		}

		DispObj->SetArrayField(TEXT("parameters"), ParamsArr);
		DispatchersArr.Add(MakeShared<FJsonValueObject>(DispObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("count"), DispatchersArr.Num());
	Result->SetArrayField(TEXT("dispatchers"), DispatchersArr);
	return JsonToString(Result);
}
