#include "BlueprintMCPServer.h"
#include "Engine/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/StructureFactory.h"
#include "Factories/EnumFactory.h"

// ============================================================
// HandleCreateStruct — create a new UserDefinedStruct asset
// ============================================================

FString FBlueprintMCPServer::HandleCreateStruct(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: assetPath (e.g. '/Game/DataTypes/S_MyStruct')"));
	}

	// Split path into package path and asset name
	FString PackagePath, AssetName;
	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.Mid(LastSlash + 1);
	}
	else
	{
		return MakeErrorJson(TEXT("assetPath must be a full path (e.g. '/Game/DataTypes/S_MyStruct')"));
	}

	if (AssetName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Invalid asset name in assetPath"));
	}

	// Check if asset already exists
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData ExistingAsset = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(AssetPath + TEXT(".") + AssetName));
	if (ExistingAsset.IsValid())
	{
		return MakeErrorJson(FString::Printf(TEXT("Asset already exists at '%s'"), *AssetPath));
	}

	// Create the struct using the AssetTools factory
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	UStructureFactory* Factory = NewObject<UStructureFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UUserDefinedStruct::StaticClass(), Factory);

	if (!NewAsset)
	{
		return MakeErrorJson(TEXT("Failed to create UserDefinedStruct asset"));
	}

	UUserDefinedStruct* NewStruct = Cast<UUserDefinedStruct>(NewAsset);
	if (!NewStruct)
	{
		return MakeErrorJson(TEXT("Created asset is not a UserDefinedStruct"));
	}

	// Add properties if specified
	const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
	int32 PropsAdded = 0;
	if (Json->TryGetArrayField(TEXT("properties"), PropsArray) && PropsArray)
	{
		for (const TSharedPtr<FJsonValue>& PropVal : *PropsArray)
		{
			TSharedPtr<FJsonObject> PropObj = PropVal->AsObject();
			if (!PropObj) continue;

			FString PropName = PropObj->GetStringField(TEXT("name"));
			FString PropType = PropObj->GetStringField(TEXT("type"));
			if (PropName.IsEmpty() || PropType.IsEmpty()) continue;

			FEdGraphPinType PinType;
			FString TypeError;
			if (!ResolveTypeFromString(PropType, PinType, TypeError))
			{
				UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Could not resolve type '%s' for property '%s': %s"), *PropType, *PropName, *TypeError);
				continue;
			}

			// Snapshot existing GUIDs so we can find the newly added one
			TSet<FGuid> ExistingGuids;
			for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(NewStruct))
			{
				ExistingGuids.Add(Var.VarGuid);
			}

			bool bAdded = FStructureEditorUtils::AddVariable(NewStruct, PinType);
			if (bAdded)
			{
				// Find the new variable by diffing GUID sets
				FGuid NewPropGuid;
				for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(NewStruct))
				{
					if (!ExistingGuids.Contains(Var.VarGuid))
					{
						NewPropGuid = Var.VarGuid;
						break;
					}
				}
				if (NewPropGuid.IsValid())
				{
					FStructureEditorUtils::RenameVariable(NewStruct, NewPropGuid, PropName);
				}
				PropsAdded++;
			}
		}
	}

	// Save
	UPackage* Package = NewStruct->GetPackage();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewStruct, *PackageFilename, SaveArgs);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created UserDefinedStruct '%s' with %d properties, save %s"),
		*AssetPath, PropsAdded, bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("assetName"), AssetName);
	Result->SetNumberField(TEXT("propertiesAdded"), PropsAdded);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleCreateEnum — create a new UserDefinedEnum asset
// ============================================================

FString FBlueprintMCPServer::HandleCreateEnum(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	if (AssetPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: assetPath (e.g. '/Game/DataTypes/E_MyEnum')"));
	}

	// Split path
	FString PackagePath, AssetName;
	int32 LastSlash;
	if (AssetPath.FindLastChar('/', LastSlash))
	{
		PackagePath = AssetPath.Left(LastSlash);
		AssetName = AssetPath.Mid(LastSlash + 1);
	}
	else
	{
		return MakeErrorJson(TEXT("assetPath must be a full path (e.g. '/Game/DataTypes/E_MyEnum')"));
	}

	if (AssetName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Invalid asset name in assetPath"));
	}

	// Get values
	const TArray<TSharedPtr<FJsonValue>>* ValuesArray = nullptr;
	if (!Json->TryGetArrayField(TEXT("values"), ValuesArray) || !ValuesArray || ValuesArray->Num() == 0)
	{
		return MakeErrorJson(TEXT("Missing or empty required field: values (array of strings)"));
	}

	TArray<FString> EnumValues;
	for (const TSharedPtr<FJsonValue>& Val : *ValuesArray)
	{
		FString Str = Val->AsString();
		if (!Str.IsEmpty()) EnumValues.Add(Str);
	}
	if (EnumValues.Num() == 0)
	{
		return MakeErrorJson(TEXT("No valid enum values provided"));
	}

	// Create the enum using AssetTools
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	IAssetTools& AssetTools = AssetToolsModule.Get();

	UEnumFactory* Factory = NewObject<UEnumFactory>();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UUserDefinedEnum::StaticClass(), Factory);

	if (!NewAsset)
	{
		return MakeErrorJson(TEXT("Failed to create UserDefinedEnum asset"));
	}

	UUserDefinedEnum* NewEnum = Cast<UUserDefinedEnum>(NewAsset);
	if (!NewEnum)
	{
		return MakeErrorJson(TEXT("Created asset is not a UserDefinedEnum"));
	}

	// Add enum values — UUserDefinedEnum starts with a MAX value.
	// We need to add entries before MAX.
	for (int32 i = 0; i < EnumValues.Num(); ++i)
	{
		// AddNewEnumeratorForUserDefinedEnum adds before the _MAX entry (returns void)
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(NewEnum);
		// The new entry is at index (NumEnums - 2) because _MAX is last
		int32 NewIndex = NewEnum->NumEnums() - 2;
		FEnumEditorUtils::SetEnumeratorDisplayName(NewEnum, NewIndex, FText::FromString(EnumValues[i]));
	}

	// Save
	UPackage* Package = NewEnum->GetPackage();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, NewEnum, *PackageFilename, SaveArgs);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created UserDefinedEnum '%s' with %d values, save %s"),
		*AssetPath, EnumValues.Num(), bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("assetName"), AssetName);
	Result->SetNumberField(TEXT("valueCount"), EnumValues.Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleAddStructProperty — add a property to UserDefinedStruct
// ============================================================

FString FBlueprintMCPServer::HandleAddStructProperty(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	FString PropName = Json->GetStringField(TEXT("name"));
	FString PropType = Json->GetStringField(TEXT("type"));

	if (AssetPath.IsEmpty() || PropName.IsEmpty() || PropType.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: assetPath, name, type"));
	}

	// Find the struct
	UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *AssetPath);
	if (!Struct)
	{
		// Try with asset name appended
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		Struct = LoadObject<UUserDefinedStruct>(nullptr, *FullPath);
	}
	if (!Struct)
	{
		return MakeErrorJson(FString::Printf(TEXT("UserDefinedStruct not found at '%s'"), *AssetPath));
	}

	// Resolve type
	FEdGraphPinType PinType;
	FString TypeError;
	if (!ResolveTypeFromString(PropType, PinType, TypeError))
	{
		return MakeErrorJson(FString::Printf(TEXT("Cannot resolve type '%s': %s"), *PropType, *TypeError));
	}

	// Snapshot existing GUIDs so we can find the newly added one
	TSet<FGuid> ExistingGuids;
	for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
	{
		ExistingGuids.Add(Var.VarGuid);
	}

	bool bAdded = FStructureEditorUtils::AddVariable(Struct, PinType);
	if (!bAdded)
	{
		return MakeErrorJson(TEXT("Failed to add property to struct"));
	}

	// Find the new variable by diffing GUID sets and rename it
	for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (!ExistingGuids.Contains(Var.VarGuid))
		{
			FStructureEditorUtils::RenameVariable(Struct, Var.VarGuid, PropName);
			break;
		}
	}

	// Save
	UPackage* Package = Struct->GetPackage();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, Struct, *PackageFilename, SaveArgs);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added property '%s' (%s) to struct '%s', save %s"),
		*PropName, *PropType, *AssetPath, bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("propertyName"), PropName);
	Result->SetStringField(TEXT("propertyType"), PropType);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveStructProperty — remove a property from UserDefinedStruct
// ============================================================

FString FBlueprintMCPServer::HandleRemoveStructProperty(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	FString PropName = Json->GetStringField(TEXT("name"));

	if (AssetPath.IsEmpty() || PropName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: assetPath, name"));
	}

	// Find the struct
	UUserDefinedStruct* Struct = LoadObject<UUserDefinedStruct>(nullptr, *AssetPath);
	if (!Struct)
	{
		FString FullPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		Struct = LoadObject<UUserDefinedStruct>(nullptr, *FullPath);
	}
	if (!Struct)
	{
		return MakeErrorJson(FString::Printf(TEXT("UserDefinedStruct not found at '%s'"), *AssetPath));
	}

	// Find the property GUID by name
	FGuid TargetGuid;
	bool bFound = false;
	for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
	{
		if (Var.FriendlyName == PropName || Var.VarName.ToString() == PropName)
		{
			TargetGuid = Var.VarGuid;
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		// List available properties
		TArray<TSharedPtr<FJsonValue>> AvailProps;
		for (const FStructVariableDescription& Var : FStructureEditorUtils::GetVarDesc(Struct))
		{
			AvailProps.Add(MakeShared<FJsonValueString>(Var.FriendlyName));
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Property '%s' not found in struct '%s'"), *PropName, *AssetPath));
		E->SetArrayField(TEXT("availableProperties"), AvailProps);
		return JsonToString(E);
	}

	bool bRemoved = FStructureEditorUtils::RemoveVariable(Struct, TargetGuid);
	if (!bRemoved)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to remove property '%s'"), *PropName));
	}

	// Save
	UPackage* Package = Struct->GetPackage();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	bool bSaved = UPackage::SavePackage(Package, Struct, *PackageFilename, SaveArgs);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed property '%s' from struct '%s', save %s"),
		*PropName, *AssetPath, bSaved ? TEXT("succeeded") : TEXT("failed"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("assetPath"), AssetPath);
	Result->SetStringField(TEXT("removedProperty"), PropName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
