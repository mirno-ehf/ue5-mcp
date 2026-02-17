#include "BlueprintMCPServer.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"
#include "Engine/Texture.h"

// ============================================================
// HandleCreateMaterialInstance — create a new Material Instance Constant
// ============================================================

FString FBlueprintMCPServer::HandleCreateMaterialInstance(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString Name = Json->GetStringField(TEXT("name"));
	FString PackagePath = Json->GetStringField(TEXT("packagePath"));
	FString ParentMaterialName = Json->GetStringField(TEXT("parentMaterial"));

	if (Name.IsEmpty() || PackagePath.IsEmpty() || ParentMaterialName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: name, packagePath, parentMaterial"));
	}

	// Validate packagePath starts with /Game
	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return MakeErrorJson(TEXT("packagePath must start with '/Game'"));
	}

	// Check if asset already exists
	FString FullAssetPath = PackagePath / Name;
	if (FindMaterialInstanceAsset(Name) || FindMaterialInstanceAsset(FullAssetPath))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Material Instance '%s' already exists. Use a different name or delete the existing asset first."),
			*Name));
	}

	// Load parent material — try as Material first, then as Material Instance
	UMaterialInterface* ParentMaterial = nullptr;
	{
		FString LoadError;
		UMaterial* ParentMat = LoadMaterialByName(ParentMaterialName, LoadError);
		if (ParentMat)
		{
			ParentMaterial = ParentMat;
		}
		else
		{
			FString MILoadError;
			UMaterialInstanceConstant* ParentMI = LoadMaterialInstanceByName(ParentMaterialName, MILoadError);
			if (ParentMI)
			{
				ParentMaterial = ParentMI;
			}
		}
	}

	if (!ParentMaterial)
	{
		// Also try LoadObject as a fallback with the raw path
		ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentMaterialName);
	}

	if (!ParentMaterial)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Parent material '%s' not found. Provide a Material or Material Instance name/path."),
			*ParentMaterialName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating Material Instance '%s' in '%s' with parent '%s'"),
		*Name, *PackagePath, *ParentMaterial->GetName());

	// Create via factory + AssetTools
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();

	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
	if (!NewAsset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to create Material Instance asset '%s' in '%s'"), *Name, *PackagePath));
	}

	UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(NewAsset);
	if (!MI)
	{
		return MakeErrorJson(TEXT("Created asset is not a UMaterialInstanceConstant"));
	}

	// Set parent
	MI->PreEditChange(nullptr);
	MI->Parent = ParentMaterial;
	MI->PostEditChange();

	// Save
	bool bSaved = SaveGenericPackage(MI);

	// Refresh asset cache
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AllMaterialInstanceAssets.Empty();
	ARM.Get().GetAssetsByClass(UMaterialInstanceConstant::StaticClass()->GetClassPathName(), AllMaterialInstanceAssets, false);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created Material Instance '%s' with parent '%s' (saved: %s)"),
		*Name, *ParentMaterial->GetName(), bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), MI->GetPathName());
	Result->SetStringField(TEXT("parent"), ParentMaterial->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleSetMaterialInstanceParameter — set a parameter override on an MI
// ============================================================

FString FBlueprintMCPServer::HandleSetMaterialInstanceParameter(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MIName = Json->GetStringField(TEXT("materialInstance"));
	FString ParamName = Json->GetStringField(TEXT("parameterName"));

	if (MIName.IsEmpty() || ParamName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: materialInstance, parameterName"));
	}

	if (!Json->HasField(TEXT("value")))
	{
		return MakeErrorJson(TEXT("Missing required field: value"));
	}

	bool bDryRun = false;
	if (Json->HasField(TEXT("dryRun")))
	{
		bDryRun = Json->GetBoolField(TEXT("dryRun"));
	}

	// Load the Material Instance
	FString LoadError;
	UMaterialInstanceConstant* MI = LoadMaterialInstanceByName(MIName, LoadError);
	if (!MI)
	{
		return MakeErrorJson(LoadError);
	}

	// Determine the parameter type — explicit or auto-detect from parent
	FString TypeStr;
	if (Json->HasField(TEXT("type")))
	{
		TypeStr = Json->GetStringField(TEXT("type"));
	}

	// Auto-detect type from parent material's parameters if not provided
	if (TypeStr.IsEmpty())
	{
		UMaterialInterface* ParentMat = MI->Parent;
		while (ParentMat)
		{
			UMaterial* BaseMat = ParentMat->GetMaterial();
			if (BaseMat)
			{
				// Check scalar parameters
				for (UMaterialExpression* Expr : BaseMat->GetExpressions())
				{
					if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
					{
						if (SP->ParameterName.ToString() == ParamName)
						{
							TypeStr = TEXT("scalar");
							break;
						}
					}
					else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
					{
						if (VP->ParameterName.ToString() == ParamName)
						{
							TypeStr = TEXT("vector");
							break;
						}
					}
					else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
					{
						if (TP->ParameterName.ToString() == ParamName)
						{
							TypeStr = TEXT("texture");
							break;
						}
					}
					else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
					{
						if (SSP->ParameterName.ToString() == ParamName)
						{
							TypeStr = TEXT("staticSwitch");
							break;
						}
					}
				}
				break; // Only need to check the base material
			}
			// Walk up the parent chain if it's an MI parented to another MI
			UMaterialInstanceConstant* ParentMI = Cast<UMaterialInstanceConstant>(ParentMat);
			if (ParentMI)
			{
				ParentMat = ParentMI->Parent;
			}
			else
			{
				break;
			}
		}
	}

	if (TypeStr.IsEmpty())
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Could not determine parameter type for '%s'. Specify the 'type' field explicitly (scalar, vector, texture, staticSwitch)."),
			*ParamName));
	}

	FString NewValueDescription;
	FMaterialParameterInfo ParamInfo(*ParamName);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %s parameter '%s' (type=%s) on Material Instance '%s'"),
		bDryRun ? TEXT("[DRY RUN] Setting") : TEXT("Setting"),
		*ParamName, *TypeStr, *MIName);

	if (TypeStr.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
	{
		// Scalar parameter — value is a number
		double FloatValue = Json->GetNumberField(TEXT("value"));

		if (!bDryRun)
		{
			MI->SetScalarParameterValueEditorOnly(ParamInfo, (float)FloatValue);
		}
		NewValueDescription = FString::Printf(TEXT("%f"), FloatValue);
	}
	else if (TypeStr.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
	{
		// Vector parameter — value is { r, g, b, a? }
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (!Json->TryGetObjectField(TEXT("value"), ValueObj) || !ValueObj || !(*ValueObj).IsValid())
		{
			return MakeErrorJson(TEXT("For vector parameters, 'value' must be an object with r, g, b (and optional a) fields."));
		}

		double R = (*ValueObj)->GetNumberField(TEXT("r"));
		double G = (*ValueObj)->GetNumberField(TEXT("g"));
		double B = (*ValueObj)->GetNumberField(TEXT("b"));
		double A = (*ValueObj)->HasField(TEXT("a")) ? (*ValueObj)->GetNumberField(TEXT("a")) : 1.0;

		FLinearColor Color((float)R, (float)G, (float)B, (float)A);

		if (!bDryRun)
		{
			MI->SetVectorParameterValueEditorOnly(ParamInfo, Color);
		}
		NewValueDescription = FString::Printf(TEXT("(R=%f, G=%f, B=%f, A=%f)"), R, G, B, A);
	}
	else if (TypeStr.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
	{
		// Texture parameter — value is a texture path string
		FString TexturePath = Json->GetStringField(TEXT("value"));
		if (TexturePath.IsEmpty())
		{
			return MakeErrorJson(TEXT("For texture parameters, 'value' must be a texture asset path string."));
		}

		UTexture* TextureObj = LoadObject<UTexture>(nullptr, *TexturePath);
		if (!TextureObj)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not load texture at path '%s'"), *TexturePath));
		}

		if (!bDryRun)
		{
			MI->SetTextureParameterValueEditorOnly(ParamInfo, TextureObj);
		}
		NewValueDescription = TexturePath;
	}
	else if (TypeStr.Equals(TEXT("staticSwitch"), ESearchCase::IgnoreCase))
	{
		// Static switch parameter — value is a bool
		bool bSwitchValue = Json->GetBoolField(TEXT("value"));

		if (!bDryRun)
		{
			// Modify static parameters
			FStaticParameterSet StaticParams;
			MI->GetStaticParameterValues(StaticParams);

			bool bFound = false;
			for (FStaticSwitchParameter& Param : StaticParams.StaticSwitchParameters)
			{
				if (Param.ParameterInfo.Name == FName(*ParamName))
				{
					Param.Value = bSwitchValue;
					Param.bOverride = true;
					bFound = true;
					break;
				}
			}

			if (!bFound)
			{
				// Add new static switch parameter entry
				FStaticSwitchParameter NewParam;
				NewParam.ParameterInfo.Name = FName(*ParamName);
				NewParam.Value = bSwitchValue;
				NewParam.bOverride = true;
				StaticParams.StaticSwitchParameters.Add(NewParam);
			}

			MI->UpdateStaticPermutation(StaticParams);
		}
		NewValueDescription = bSwitchValue ? TEXT("true") : TEXT("false");
	}
	else
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Unknown parameter type '%s'. Valid types: scalar, vector, texture, staticSwitch"),
			*TypeStr));
	}

	if (!bDryRun)
	{
		MI->PreEditChange(nullptr);
		MI->PostEditChange();
		MI->MarkPackageDirty();
		SaveGenericPackage(MI);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %s parameter '%s' = %s on '%s'"),
		bDryRun ? TEXT("[DRY RUN] Would set") : TEXT("Set"),
		*ParamName, *NewValueDescription, *MIName);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("materialInstance"), MIName);
	Result->SetStringField(TEXT("parameterName"), ParamName);
	Result->SetStringField(TEXT("type"), TypeStr);
	Result->SetStringField(TEXT("newValue"), NewValueDescription);
	if (bDryRun)
	{
		Result->SetBoolField(TEXT("dryRun"), true);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleGetMaterialInstanceParameters — list all parameters on an MI
// ============================================================

FString FBlueprintMCPServer::HandleGetMaterialInstanceParameters(const TMap<FString, FString>& Params)
{
	const FString* NameParam = Params.Find(TEXT("name"));
	if (!NameParam || NameParam->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required query parameter: name"));
	}

	FString LoadError;
	UMaterialInstanceConstant* MI = LoadMaterialInstanceByName(*NameParam, LoadError);
	if (!MI)
	{
		return MakeErrorJson(LoadError);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), MI->GetName());
	Result->SetStringField(TEXT("path"), MI->GetPathName());

	// Parent info
	if (MI->Parent)
	{
		Result->SetStringField(TEXT("parent"), MI->Parent->GetPathName());
	}

	// Build parent chain
	TArray<TSharedPtr<FJsonValue>> ParentChainArr;
	{
		UMaterialInterface* Current = MI->Parent;
		while (Current)
		{
			TSharedRef<FJsonObject> ParentObj = MakeShared<FJsonObject>();
			ParentObj->SetStringField(TEXT("name"), Current->GetName());
			ParentObj->SetStringField(TEXT("path"), Current->GetPathName());
			ParentObj->SetStringField(TEXT("class"), Current->GetClass()->GetName());
			ParentChainArr.Add(MakeShared<FJsonValueObject>(ParentObj));

			UMaterialInstanceConstant* ParentMI = Cast<UMaterialInstanceConstant>(Current);
			if (ParentMI)
			{
				Current = ParentMI->Parent;
			}
			else
			{
				break; // Reached the root Material
			}
		}
	}
	Result->SetArrayField(TEXT("parentChain"), ParentChainArr);

	// Scalar parameters
	TArray<TSharedPtr<FJsonValue>> ScalarArr;
	for (const FScalarParameterValue& Param : MI->ScalarParameterValues)
	{
		TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PObj->SetNumberField(TEXT("value"), Param.ParameterValue);
		PObj->SetBoolField(TEXT("isOverridden"), true); // Present in ScalarParameterValues means it's overridden
		ScalarArr.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Result->SetArrayField(TEXT("scalarParameters"), ScalarArr);

	// Vector parameters
	TArray<TSharedPtr<FJsonValue>> VectorArr;
	for (const FVectorParameterValue& Param : MI->VectorParameterValues)
	{
		TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		PObj->SetNumberField(TEXT("r"), Param.ParameterValue.R);
		PObj->SetNumberField(TEXT("g"), Param.ParameterValue.G);
		PObj->SetNumberField(TEXT("b"), Param.ParameterValue.B);
		PObj->SetNumberField(TEXT("a"), Param.ParameterValue.A);
		PObj->SetBoolField(TEXT("isOverridden"), true);
		VectorArr.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Result->SetArrayField(TEXT("vectorParameters"), VectorArr);

	// Texture parameters
	TArray<TSharedPtr<FJsonValue>> TextureArr;
	for (const FTextureParameterValue& Param : MI->TextureParameterValues)
	{
		TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
		PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
		if (Param.ParameterValue)
		{
			PObj->SetStringField(TEXT("texture"), Param.ParameterValue->GetPathName());
		}
		else
		{
			PObj->SetStringField(TEXT("texture"), TEXT("None"));
		}
		PObj->SetBoolField(TEXT("isOverridden"), true);
		TextureArr.Add(MakeShared<FJsonValueObject>(PObj));
	}
	Result->SetArrayField(TEXT("textureParameters"), TextureArr);

	// Static switch parameters
	TArray<TSharedPtr<FJsonValue>> StaticSwitchArr;
	{
		FStaticParameterSet StaticParams;
		MI->GetStaticParameterValues(StaticParams);

		for (const FStaticSwitchParameter& Param : StaticParams.StaticSwitchParameters)
		{
			TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			PObj->SetBoolField(TEXT("value"), Param.Value);
			PObj->SetBoolField(TEXT("isOverridden"), Param.bOverride);
			StaticSwitchArr.Add(MakeShared<FJsonValueObject>(PObj));
		}
	}
	Result->SetArrayField(TEXT("staticSwitchParameters"), StaticSwitchArr);

	// Also report inherited parameters from the parent material for discoverability
	TArray<TSharedPtr<FJsonValue>> InheritedScalarArr;
	TArray<TSharedPtr<FJsonValue>> InheritedVectorArr;
	TArray<TSharedPtr<FJsonValue>> InheritedTextureArr;
	TArray<TSharedPtr<FJsonValue>> InheritedStaticSwitchArr;
	{
		UMaterial* BaseMat = MI->GetMaterial();
		if (BaseMat)
		{
			// Collect names of already-overridden parameters for filtering
			TSet<FString> OverriddenScalars;
			for (const FScalarParameterValue& P : MI->ScalarParameterValues)
			{
				OverriddenScalars.Add(P.ParameterInfo.Name.ToString());
			}
			TSet<FString> OverriddenVectors;
			for (const FVectorParameterValue& P : MI->VectorParameterValues)
			{
				OverriddenVectors.Add(P.ParameterInfo.Name.ToString());
			}
			TSet<FString> OverriddenTextures;
			for (const FTextureParameterValue& P : MI->TextureParameterValues)
			{
				OverriddenTextures.Add(P.ParameterInfo.Name.ToString());
			}
			TSet<FString> OverriddenStaticSwitches;
			{
				FStaticParameterSet SP;
				MI->GetStaticParameterValues(SP);
				for (const FStaticSwitchParameter& P : SP.StaticSwitchParameters)
				{
					if (P.bOverride)
					{
						OverriddenStaticSwitches.Add(P.ParameterInfo.Name.ToString());
					}
				}
			}

			for (UMaterialExpression* Expr : BaseMat->GetExpressions())
			{
				if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				{
					if (!OverriddenScalars.Contains(SP->ParameterName.ToString()))
					{
						TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
						PObj->SetStringField(TEXT("name"), SP->ParameterName.ToString());
						PObj->SetNumberField(TEXT("defaultValue"), SP->DefaultValue);
						PObj->SetBoolField(TEXT("isOverridden"), false);
						InheritedScalarArr.Add(MakeShared<FJsonValueObject>(PObj));
					}
				}
				else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				{
					if (!OverriddenVectors.Contains(VP->ParameterName.ToString()))
					{
						TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
						PObj->SetStringField(TEXT("name"), VP->ParameterName.ToString());
						PObj->SetNumberField(TEXT("r"), VP->DefaultValue.R);
						PObj->SetNumberField(TEXT("g"), VP->DefaultValue.G);
						PObj->SetNumberField(TEXT("b"), VP->DefaultValue.B);
						PObj->SetNumberField(TEXT("a"), VP->DefaultValue.A);
						PObj->SetBoolField(TEXT("isOverridden"), false);
						InheritedVectorArr.Add(MakeShared<FJsonValueObject>(PObj));
					}
				}
				else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				{
					if (!OverriddenTextures.Contains(TP->ParameterName.ToString()))
					{
						TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
						PObj->SetStringField(TEXT("name"), TP->ParameterName.ToString());
						if (TP->Texture)
						{
							PObj->SetStringField(TEXT("defaultTexture"), TP->Texture->GetPathName());
						}
						else
						{
							PObj->SetStringField(TEXT("defaultTexture"), TEXT("None"));
						}
						PObj->SetBoolField(TEXT("isOverridden"), false);
						InheritedTextureArr.Add(MakeShared<FJsonValueObject>(PObj));
					}
				}
				else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					if (!OverriddenStaticSwitches.Contains(SSP->ParameterName.ToString()))
					{
						TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
						PObj->SetStringField(TEXT("name"), SSP->ParameterName.ToString());
						PObj->SetBoolField(TEXT("defaultValue"), SSP->DefaultValue);
						PObj->SetBoolField(TEXT("isOverridden"), false);
						InheritedStaticSwitchArr.Add(MakeShared<FJsonValueObject>(PObj));
					}
				}
			}
		}
	}

	// Merge inherited (non-overridden) params into the arrays
	for (const TSharedPtr<FJsonValue>& V : InheritedScalarArr)
	{
		ScalarArr.Add(V);
	}
	for (const TSharedPtr<FJsonValue>& V : InheritedVectorArr)
	{
		VectorArr.Add(V);
	}
	for (const TSharedPtr<FJsonValue>& V : InheritedTextureArr)
	{
		TextureArr.Add(V);
	}
	for (const TSharedPtr<FJsonValue>& V : InheritedStaticSwitchArr)
	{
		StaticSwitchArr.Add(V);
	}

	// Update arrays with merged data
	Result->SetArrayField(TEXT("scalarParameters"), ScalarArr);
	Result->SetArrayField(TEXT("vectorParameters"), VectorArr);
	Result->SetArrayField(TEXT("textureParameters"), TextureArr);
	Result->SetArrayField(TEXT("staticSwitchParameters"), StaticSwitchArr);

	return JsonToString(Result);
}

// ============================================================
// HandleReparentMaterialInstance — change parent of an MI
// ============================================================

FString FBlueprintMCPServer::HandleReparentMaterialInstance(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MIName = Json->GetStringField(TEXT("materialInstance"));
	FString NewParentName = Json->GetStringField(TEXT("newParent"));

	if (MIName.IsEmpty() || NewParentName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: materialInstance, newParent"));
	}

	bool bDryRun = false;
	if (Json->HasField(TEXT("dryRun")))
	{
		bDryRun = Json->GetBoolField(TEXT("dryRun"));
	}

	// Load the Material Instance
	FString LoadError;
	UMaterialInstanceConstant* MI = LoadMaterialInstanceByName(MIName, LoadError);
	if (!MI)
	{
		return MakeErrorJson(LoadError);
	}

	// Capture old parent
	FString OldParentPath = MI->Parent ? MI->Parent->GetPathName() : TEXT("None");

	// Load new parent — try as Material first, then as Material Instance
	UMaterialInterface* NewParent = nullptr;
	{
		FString MatLoadError;
		UMaterial* NewParentMat = LoadMaterialByName(NewParentName, MatLoadError);
		if (NewParentMat)
		{
			NewParent = NewParentMat;
		}
		else
		{
			FString MILoadError;
			UMaterialInstanceConstant* NewParentMI = LoadMaterialInstanceByName(NewParentName, MILoadError);
			if (NewParentMI)
			{
				NewParent = NewParentMI;
			}
		}
	}

	if (!NewParent)
	{
		// Try LoadObject as a fallback
		NewParent = LoadObject<UMaterialInterface>(nullptr, *NewParentName);
	}

	if (!NewParent)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("New parent material '%s' not found. Provide a Material or Material Instance name/path."),
			*NewParentName));
	}

	// Prevent circular parenting — check if NewParent is this MI or has this MI in its chain
	{
		UMaterialInterface* Check = NewParent;
		while (Check)
		{
			if (Check == MI)
			{
				return MakeErrorJson(FString::Printf(
					TEXT("Cannot reparent '%s' to '%s' — this would create a circular parent chain."),
					*MIName, *NewParentName));
			}
			UMaterialInstanceConstant* CheckMI = Cast<UMaterialInstanceConstant>(Check);
			if (CheckMI)
			{
				Check = CheckMI->Parent;
			}
			else
			{
				break;
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %s Material Instance '%s': parent '%s' -> '%s'"),
		bDryRun ? TEXT("[DRY RUN] Reparenting") : TEXT("Reparenting"),
		*MIName, *OldParentPath, *NewParent->GetPathName());

	if (!bDryRun)
	{
		MI->PreEditChange(nullptr);
		MI->Parent = NewParent;
		MI->PostEditChange();

		bool bSaved = SaveGenericPackage(MI);

		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Reparented Material Instance '%s' (saved: %s)"),
			*MIName, bSaved ? TEXT("true") : TEXT("false"));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("materialInstance"), MIName);
	Result->SetStringField(TEXT("oldParent"), OldParentPath);
	Result->SetStringField(TEXT("newParent"), NewParent->GetPathName());
	if (bDryRun)
	{
		Result->SetBoolField(TEXT("dryRun"), true);
	}
	return JsonToString(Result);
}
