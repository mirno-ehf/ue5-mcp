#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

// ============================================================
// HandleGetPinInfo — detailed information about a specific pin
// ============================================================

FString FBlueprintMCPServer::HandleGetPinInfo(const FString& Body)
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

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UEdGraph* Graph = nullptr;
	UEdGraphNode* Node = FindNodeByGuid(BP, NodeId, &Graph);
	if (!Node)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
	}

	UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
	if (!Pin)
	{
		// List available pins
		TArray<TSharedPtr<FJsonValue>> AvailPins;
		for (UEdGraphPin* P : Node->Pins)
		{
			if (P)
			{
				TSharedRef<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("name"), P->PinName.ToString());
				PinObj->SetStringField(TEXT("direction"), P->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
				PinObj->SetStringField(TEXT("type"), P->PinType.PinCategory.ToString());
				AvailPins.Add(MakeShared<FJsonValueObject>(PinObj));
			}
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId));
		E->SetArrayField(TEXT("availablePins"), AvailPins);
		return JsonToString(E);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	Result->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"));
	Result->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

	if (!Pin->PinType.PinSubCategory.IsNone())
	{
		Result->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
	}
	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		Result->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategoryObject->GetName());
	}

	Result->SetBoolField(TEXT("isArray"), Pin->PinType.IsArray());
	Result->SetBoolField(TEXT("isSet"), Pin->PinType.IsSet());
	Result->SetBoolField(TEXT("isMap"), Pin->PinType.IsMap());
	Result->SetBoolField(TEXT("isReference"), Pin->PinType.bIsReference);
	Result->SetBoolField(TEXT("isConst"), Pin->PinType.bIsConst);

	if (!Pin->DefaultValue.IsEmpty())
	{
		Result->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
	}
	if (!Pin->DefaultTextValue.IsEmpty())
	{
		Result->SetStringField(TEXT("defaultTextValue"), Pin->DefaultTextValue.ToString());
	}
	if (Pin->DefaultObject)
	{
		Result->SetStringField(TEXT("defaultObject"), Pin->DefaultObject->GetPathName());
	}

	// Connected pins
	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Conns;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (!Linked || !Linked->GetOwningNode()) continue;
			TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
			CJ->SetStringField(TEXT("nodeId"), Linked->GetOwningNode()->NodeGuid.ToString());
			CJ->SetStringField(TEXT("pinName"), Linked->PinName.ToString());
			CJ->SetStringField(TEXT("nodeTitle"), Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			Conns.Add(MakeShared<FJsonValueObject>(CJ));
		}
		Result->SetArrayField(TEXT("connectedTo"), Conns);
	}

	return JsonToString(Result);
}

// ============================================================
// HandleCheckPinCompatibility — pre-flight check for connect_pins
// ============================================================

FString FBlueprintMCPServer::HandleCheckPinCompatibility(const FString& Body)
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

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UEdGraph* SourceGraph = nullptr;
	UEdGraphNode* SourceNode = FindNodeByGuid(BP, SourceNodeId, &SourceGraph);
	if (!SourceNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId));
	}

	UEdGraphNode* TargetNode = FindNodeByGuid(BP, TargetNodeId);
	if (!TargetNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));
	}

	UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
	if (!SourcePin)
	{
		return MakeErrorJson(FString::Printf(TEXT("Source pin '%s' not found on node '%s'"), *SourcePinName, *SourceNodeId));
	}

	UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
	if (!TargetPin)
	{
		return MakeErrorJson(FString::Printf(TEXT("Target pin '%s' not found on node '%s'"), *TargetPinName, *TargetNodeId));
	}

	const UEdGraphSchema* Schema = SourceGraph ? SourceGraph->GetSchema() : nullptr;
	if (!Schema)
	{
		return MakeErrorJson(TEXT("Graph schema not found"));
	}

	// Check compatibility using the schema
	const FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);

	bool bCompatible = (Response.Response != ECanCreateConnectionResponse::CONNECT_RESPONSE_DISALLOW);
	Result->SetBoolField(TEXT("compatible"), bCompatible);

	// Decode the response type
	FString ResponseType;
	switch (Response.Response)
	{
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_MAKE:
		ResponseType = TEXT("direct");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_BREAK_OTHERS_A:
		ResponseType = TEXT("breakSourceConnections");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_BREAK_OTHERS_B:
		ResponseType = TEXT("breakTargetConnections");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_BREAK_OTHERS_AB:
		ResponseType = TEXT("breakBothConnections");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE:
		ResponseType = TEXT("requiresConversion");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_MAKE_WITH_PROMOTION:
		ResponseType = TEXT("requiresPromotion");
		break;
	case ECanCreateConnectionResponse::CONNECT_RESPONSE_DISALLOW:
	default:
		ResponseType = TEXT("disallowed");
		break;
	}
	Result->SetStringField(TEXT("connectionType"), ResponseType);

	if (!Response.Message.IsEmpty())
	{
		Result->SetStringField(TEXT("message"), Response.Message.ToString());
	}

	// Include pin type info for context
	Result->SetStringField(TEXT("sourcePinType"), SourcePin->PinType.PinCategory.ToString());
	if (SourcePin->PinType.PinSubCategoryObject.IsValid())
		Result->SetStringField(TEXT("sourcePinSubtype"), SourcePin->PinType.PinSubCategoryObject->GetName());
	Result->SetStringField(TEXT("targetPinType"), TargetPin->PinType.PinCategory.ToString());
	if (TargetPin->PinType.PinSubCategoryObject.IsValid())
		Result->SetStringField(TEXT("targetPinSubtype"), TargetPin->PinType.PinSubCategoryObject->GetName());

	return JsonToString(Result);
}

// ============================================================
// HandleListClasses — discover available UClasses
// ============================================================

FString FBlueprintMCPServer::HandleListClasses(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString Filter = Json->GetStringField(TEXT("filter"));
	FString ParentClassName = Json->GetStringField(TEXT("parentClass"));
	int32 Limit = 100;
	if (Json->HasField(TEXT("limit")))
	{
		Limit = FMath::Clamp(Json->GetIntegerField(TEXT("limit")), 1, 500);
	}

	UClass* ParentClass = nullptr;
	if (!ParentClassName.IsEmpty())
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ParentClassName || It->GetName() == ParentClassName + TEXT("_C"))
			{
				ParentClass = *It;
				break;
			}
		}
		if (!ParentClass)
		{
			return MakeErrorJson(FString::Printf(TEXT("Parent class '%s' not found"), *ParentClassName));
		}
	}

	TArray<TSharedPtr<FJsonValue>> ClassList;
	int32 TotalMatched = 0;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class) continue;

		// Skip internal/deprecated classes
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

		// Apply parent filter
		if (ParentClass && !Class->IsChildOf(ParentClass)) continue;

		FString ClassName = Class->GetName();

		// Apply name filter
		if (!Filter.IsEmpty() && !ClassName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TotalMatched++;
		if (ClassList.Num() >= Limit) continue; // Count but don't add beyond limit

		TSharedRef<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), ClassName);
		ClassObj->SetStringField(TEXT("fullPath"), Class->GetPathName());

		// Determine if it's a Blueprint-generated class
		bool bIsBlueprint = Class->ClassGeneratedBy != nullptr;
		ClassObj->SetBoolField(TEXT("isBlueprint"), bIsBlueprint);

		// Parent class
		if (Class->GetSuperClass())
		{
			ClassObj->SetStringField(TEXT("parentClass"), Class->GetSuperClass()->GetName());
		}

		// Module/package info
		UPackage* Package = Class->GetOuterUPackage();
		if (Package)
		{
			ClassObj->SetStringField(TEXT("package"), Package->GetName());
		}

		// Flags
		TArray<TSharedPtr<FJsonValue>> Flags;
		if (Class->HasAnyClassFlags(CLASS_Abstract)) Flags.Add(MakeShared<FJsonValueString>(TEXT("Abstract")));
		if (Class->HasAnyClassFlags(CLASS_Interface)) Flags.Add(MakeShared<FJsonValueString>(TEXT("Interface")));
		if (Class->HasAnyClassFlags(CLASS_MinimalAPI)) Flags.Add(MakeShared<FJsonValueString>(TEXT("MinimalAPI")));
		if (Flags.Num() > 0)
		{
			ClassObj->SetArrayField(TEXT("flags"), Flags);
		}

		ClassList.Add(MakeShared<FJsonValueObject>(ClassObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("count"), ClassList.Num());
	Result->SetNumberField(TEXT("totalMatched"), TotalMatched);
	if (TotalMatched > Limit)
	{
		Result->SetBoolField(TEXT("truncated"), true);
		Result->SetNumberField(TEXT("limit"), Limit);
	}
	Result->SetArrayField(TEXT("classes"), ClassList);
	return JsonToString(Result);
}

// ============================================================
// HandleListFunctions — list Blueprint-callable functions on a class
// ============================================================

FString FBlueprintMCPServer::HandleListFunctions(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString ClassName = Json->GetStringField(TEXT("className"));
	FString Filter = Json->GetStringField(TEXT("filter"));

	if (ClassName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: className"));
	}

	// Find the class
	UClass* FoundClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ClassName || It->GetName() == ClassName + TEXT("_C"))
		{
			FoundClass = *It;
			break;
		}
	}
	if (!FoundClass)
	{
		return MakeErrorJson(FString::Printf(TEXT("Class '%s' not found"), *ClassName));
	}

	TArray<TSharedPtr<FJsonValue>> FuncList;

	for (TFieldIterator<UFunction> FuncIt(FoundClass); FuncIt; ++FuncIt)
	{
		UFunction* Func = *FuncIt;
		if (!Func) continue;

		// Only include Blueprint-callable functions
		if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintPure | FUNC_BlueprintEvent)) continue;

		FString FuncName = Func->GetName();

		// Apply filter
		if (!Filter.IsEmpty() && !FuncName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), FuncName);

		// Determine the owning class
		UClass* OwnerClass = Func->GetOwnerClass();
		if (OwnerClass)
		{
			FuncObj->SetStringField(TEXT("definedIn"), OwnerClass->GetName());
		}

		// Function flags
		FuncObj->SetBoolField(TEXT("isPure"), Func->HasAnyFunctionFlags(FUNC_BlueprintPure));
		FuncObj->SetBoolField(TEXT("isStatic"), Func->HasAnyFunctionFlags(FUNC_Static));
		FuncObj->SetBoolField(TEXT("isEvent"), Func->HasAnyFunctionFlags(FUNC_BlueprintEvent));
		FuncObj->SetBoolField(TEXT("isConst"), Func->HasAnyFunctionFlags(FUNC_Const));

		// Parameters
		TArray<TSharedPtr<FJsonValue>> Params;
		FString ReturnType;
		for (TFieldIterator<FProperty> PropIt(Func); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop) continue;

			FString PropType = Prop->GetCPPType();

			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnType = PropType;
				continue;
			}

			if (Prop->HasAnyPropertyFlags(CPF_Parm))
			{
				TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
				ParamObj->SetStringField(TEXT("name"), Prop->GetName());
				ParamObj->SetStringField(TEXT("type"), PropType);
				ParamObj->SetBoolField(TEXT("isOutput"), Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ReferenceParm));
				ParamObj->SetBoolField(TEXT("isReference"), Prop->HasAnyPropertyFlags(CPF_ReferenceParm));
				Params.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
		}
		FuncObj->SetArrayField(TEXT("parameters"), Params);
		if (!ReturnType.IsEmpty())
		{
			FuncObj->SetStringField(TEXT("returnType"), ReturnType);
		}

		FuncList.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("className"), FoundClass->GetName());
	Result->SetNumberField(TEXT("count"), FuncList.Num());
	Result->SetArrayField(TEXT("functions"), FuncList);
	return JsonToString(Result);
}

// ============================================================
// HandleListProperties — list properties on a class
// ============================================================

FString FBlueprintMCPServer::HandleListProperties(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString ClassName = Json->GetStringField(TEXT("className"));
	FString Filter = Json->GetStringField(TEXT("filter"));

	if (ClassName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: className"));
	}

	// Find the class
	UClass* FoundClass = nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (It->GetName() == ClassName || It->GetName() == ClassName + TEXT("_C"))
		{
			FoundClass = *It;
			break;
		}
	}
	if (!FoundClass)
	{
		return MakeErrorJson(FString::Printf(TEXT("Class '%s' not found"), *ClassName));
	}

	TArray<TSharedPtr<FJsonValue>> PropList;

	for (TFieldIterator<FProperty> PropIt(FoundClass); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop) continue;

		FString PropName = Prop->GetName();

		// Apply filter
		if (!Filter.IsEmpty() && !PropName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), PropName);
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());

		// Determine the owning class
		UClass* OwnerClass = Prop->GetOwnerClass();
		if (OwnerClass)
		{
			PropObj->SetStringField(TEXT("definedIn"), OwnerClass->GetName());
		}

		// Property flags
		TArray<TSharedPtr<FJsonValue>> Flags;
		if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible)) Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintVisible")));
		if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly)) Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadOnly")));
		if (Prop->HasAnyPropertyFlags(CPF_Edit)) Flags.Add(MakeShared<FJsonValueString>(TEXT("EditAnywhere")));
		if (Prop->HasAnyPropertyFlags(CPF_EditConst)) Flags.Add(MakeShared<FJsonValueString>(TEXT("VisibleOnly")));
		if (Prop->HasAnyPropertyFlags(CPF_Config)) Flags.Add(MakeShared<FJsonValueString>(TEXT("Config")));
		if (Prop->HasAnyPropertyFlags(CPF_SaveGame)) Flags.Add(MakeShared<FJsonValueString>(TEXT("SaveGame")));
		if (Prop->HasAnyPropertyFlags(CPF_Transient)) Flags.Add(MakeShared<FJsonValueString>(TEXT("Transient")));
		if (Prop->HasAnyPropertyFlags(CPF_RepNotify)) Flags.Add(MakeShared<FJsonValueString>(TEXT("RepNotify")));
		if (Flags.Num() > 0)
		{
			PropObj->SetArrayField(TEXT("flags"), Flags);
		}

		PropList.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("className"), FoundClass->GetName());
	Result->SetNumberField(TEXT("count"), PropList.Num());
	Result->SetArrayField(TEXT("properties"), PropList);
	return JsonToString(Result);
}
