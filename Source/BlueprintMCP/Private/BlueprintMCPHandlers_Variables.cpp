#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

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

	// Resolve the type using the shared helper
	FEdGraphPinType PinType;
	FString TypeError;
	if (!ResolveTypeFromString(VariableType, PinType, TypeError))
	{
		return MakeErrorJson(TypeError);
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
