#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

// ============================================================
// HandleListInterfaces — list implemented interfaces on a Blueprint
// ============================================================

FString FBlueprintMCPServer::HandleListInterfaces(const FString& Body)
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

	TArray<TSharedPtr<FJsonValue>> InterfacesArr;
	for (const FBPInterfaceDescription& IfaceDesc : BP->ImplementedInterfaces)
	{
		if (!IfaceDesc.Interface)
		{
			continue;
		}

		TSharedRef<FJsonObject> IfaceObj = MakeShared<FJsonObject>();
		IfaceObj->SetStringField(TEXT("name"), IfaceDesc.Interface->GetName());
		IfaceObj->SetStringField(TEXT("classPath"), IfaceDesc.Interface->GetPathName());

		// Collect function graph names from the interface
		TArray<TSharedPtr<FJsonValue>> FuncArr;
		for (const UEdGraph* Graph : IfaceDesc.Graphs)
		{
			if (Graph)
			{
				FuncArr.Add(MakeShared<FJsonValueString>(Graph->GetName()));
			}
		}
		IfaceObj->SetArrayField(TEXT("functions"), FuncArr);

		InterfacesArr.Add(MakeShared<FJsonValueObject>(IfaceObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("count"), InterfacesArr.Num());
	Result->SetArrayField(TEXT("interfaces"), InterfacesArr);
	return JsonToString(Result);
}

// ============================================================
// HandleAddInterface — add a Blueprint Interface implementation
// ============================================================

FString FBlueprintMCPServer::HandleAddInterface(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString InterfaceName = Json->GetStringField(TEXT("interfaceName"));

	if (BlueprintName.IsEmpty() || InterfaceName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, interfaceName"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Resolve the interface class
	UClass* InterfaceClass = nullptr;

	// Strategy 1: Search loaded UInterface classes by name
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UInterface::StaticClass()))
		{
			continue;
		}

		FString ClassName = It->GetName();
		// Match by class name (e.g. "BPI_Foo_C") or by trimmed name (e.g. "BPI_Foo")
		if (ClassName.Equals(InterfaceName, ESearchCase::IgnoreCase))
		{
			InterfaceClass = *It;
			break;
		}
		// Strip the generated "_C" suffix for comparison
		FString TrimmedName = ClassName;
		if (TrimmedName.EndsWith(TEXT("_C")))
		{
			TrimmedName = TrimmedName.LeftChop(2);
		}
		if (TrimmedName.Equals(InterfaceName, ESearchCase::IgnoreCase))
		{
			InterfaceClass = *It;
			break;
		}
	}

	// Strategy 2: Try loading as a Blueprint Interface asset
	if (!InterfaceClass)
	{
		FString IfaceLoadError;
		UBlueprint* IfaceBP = LoadBlueprintByName(InterfaceName, IfaceLoadError);
		if (IfaceBP && IfaceBP->GeneratedClass && IfaceBP->GeneratedClass->IsChildOf(UInterface::StaticClass()))
		{
			InterfaceClass = IfaceBP->GeneratedClass;
		}
	}

	if (!InterfaceClass)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Interface '%s' not found. Provide a Blueprint Interface asset name (e.g. 'BPI_MyInterface') or a native UInterface class name."),
			*InterfaceName));
	}

	// Check for duplicates
	for (const FBPInterfaceDescription& IfaceDesc : BP->ImplementedInterfaces)
	{
		if (IfaceDesc.Interface == InterfaceClass)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Interface '%s' is already implemented by Blueprint '%s'"),
				*InterfaceName, *BlueprintName));
		}
	}

	// Get interface class path for the non-deprecated overload
	FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding interface '%s' to Blueprint '%s'"),
		*InterfaceClass->GetName(), *BlueprintName);

	bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(BP, InterfacePath);
	if (!bAdded)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("FBlueprintEditorUtils::ImplementNewInterface failed for interface '%s' on Blueprint '%s'"),
			*InterfaceName, *BlueprintName));
	}

	// Collect stub function graph names from the newly added interface entry
	TArray<FString> AddedFunctions;
	for (const FBPInterfaceDescription& IfaceDesc : BP->ImplementedInterfaces)
	{
		if (IfaceDesc.Interface == InterfaceClass)
		{
			for (const UEdGraph* Graph : IfaceDesc.Graphs)
			{
				if (Graph)
				{
					AddedFunctions.Add(Graph->GetName());
				}
			}
			break;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added interface '%s' to '%s' (%d function stubs, saved: %s)"),
		*InterfaceClass->GetName(), *BlueprintName, AddedFunctions.Num(), bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("interfaceName"), InterfaceClass->GetName());
	Result->SetStringField(TEXT("interfacePath"), InterfaceClass->GetPathName());

	TArray<TSharedPtr<FJsonValue>> FuncArr;
	for (const FString& FuncName : AddedFunctions)
	{
		FuncArr.Add(MakeShared<FJsonValueString>(FuncName));
	}
	Result->SetArrayField(TEXT("functionGraphsAdded"), FuncArr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveInterface — remove a Blueprint Interface implementation
// ============================================================

FString FBlueprintMCPServer::HandleRemoveInterface(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString InterfaceName = Json->GetStringField(TEXT("interfaceName"));

	if (BlueprintName.IsEmpty() || InterfaceName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, interfaceName"));
	}

	bool bPreserveFunctions = false;
	if (Json->HasField(TEXT("preserveFunctions")))
	{
		bPreserveFunctions = Json->GetBoolField(TEXT("preserveFunctions"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	// Find the interface in ImplementedInterfaces by name (case-insensitive)
	UClass* FoundInterface = nullptr;
	for (const FBPInterfaceDescription& IfaceDesc : BP->ImplementedInterfaces)
	{
		if (!IfaceDesc.Interface)
		{
			continue;
		}

		FString ClassName = IfaceDesc.Interface->GetName();
		if (ClassName.Equals(InterfaceName, ESearchCase::IgnoreCase))
		{
			FoundInterface = IfaceDesc.Interface;
			break;
		}
		// Strip "_C" suffix for comparison
		FString TrimmedName = ClassName;
		if (TrimmedName.EndsWith(TEXT("_C")))
		{
			TrimmedName = TrimmedName.LeftChop(2);
		}
		if (TrimmedName.Equals(InterfaceName, ESearchCase::IgnoreCase))
		{
			FoundInterface = IfaceDesc.Interface;
			break;
		}
	}

	if (!FoundInterface)
	{
		// Build helpful error with list of implemented interfaces
		TArray<TSharedPtr<FJsonValue>> IfaceList;
		for (const FBPInterfaceDescription& IfaceDesc : BP->ImplementedInterfaces)
		{
			if (IfaceDesc.Interface)
			{
				IfaceList.Add(MakeShared<FJsonValueString>(IfaceDesc.Interface->GetName()));
			}
		}

		TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Interface '%s' is not implemented by Blueprint '%s'"),
			*InterfaceName, *BlueprintName));
		ErrorResult->SetArrayField(TEXT("implementedInterfaces"), IfaceList);
		return JsonToString(ErrorResult);
	}

	FTopLevelAssetPath InterfacePath = FoundInterface->GetClassPathName();

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removing interface '%s' from Blueprint '%s' (preserveFunctions: %s)"),
		*FoundInterface->GetName(), *BlueprintName, bPreserveFunctions ? TEXT("true") : TEXT("false"));

	FBlueprintEditorUtils::RemoveInterface(BP, InterfacePath, bPreserveFunctions);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed interface '%s' from '%s' (saved: %s)"),
		*FoundInterface->GetName(), *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("interfaceName"), FoundInterface->GetName());
	Result->SetBoolField(TEXT("preservedFunctions"), bPreserveFunctions);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
