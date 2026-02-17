#include "BlueprintMCPServer.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionPower.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"

// ============================================================
// Phase 2: Material Mutations
// ============================================================

// ============================================================
// HandleCreateMaterial — create a new UMaterial asset
// ============================================================

FString FBlueprintMCPServer::HandleCreateMaterial(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString Name = Json->GetStringField(TEXT("name"));
	FString PackagePath = Json->GetStringField(TEXT("packagePath"));

	if (Name.IsEmpty() || PackagePath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: name, packagePath"));
	}

	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return MakeErrorJson(TEXT("packagePath must start with '/Game'"));
	}

	// Check if asset already exists
	FString FullAssetPath = PackagePath / Name;
	if (FindMaterialAsset(Name) || FindMaterialAsset(FullAssetPath))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Material '%s' already exists. Use a different name or delete the existing asset first."),
			*Name));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating Material '%s' in '%s'"), *Name, *PackagePath);

	// Create via IAssetTools + factory
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterial::StaticClass(), Factory);

	if (!NewAsset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to create Material '%s' in '%s'"), *Name, *PackagePath));
	}

	UMaterial* Material = Cast<UMaterial>(NewAsset);
	if (!Material)
	{
		return MakeErrorJson(TEXT("Created asset is not a UMaterial"));
	}

	// Apply optional properties
	FString DomainStr;
	Json->TryGetStringField(TEXT("domain"), DomainStr);

	FString BlendModeStr;
	Json->TryGetStringField(TEXT("blendMode"), BlendModeStr);

	bool bTwoSided = false;
	bool bHasTwoSided = Json->TryGetBoolField(TEXT("twoSided"), bTwoSided);

	Material->PreEditChange(nullptr);

	// Parse domain
	if (!DomainStr.IsEmpty())
	{
		if (DomainStr == TEXT("Surface"))
			Material->MaterialDomain = MD_Surface;
		else if (DomainStr == TEXT("DeferredDecal"))
			Material->MaterialDomain = MD_DeferredDecal;
		else if (DomainStr == TEXT("LightFunction"))
			Material->MaterialDomain = MD_LightFunction;
		else if (DomainStr == TEXT("Volume"))
			Material->MaterialDomain = MD_Volume;
		else if (DomainStr == TEXT("PostProcess"))
			Material->MaterialDomain = MD_PostProcess;
		else if (DomainStr == TEXT("UI"))
			Material->MaterialDomain = MD_UI;
	}

	// Parse blend mode
	if (!BlendModeStr.IsEmpty())
	{
		if (BlendModeStr == TEXT("Opaque"))
			Material->BlendMode = BLEND_Opaque;
		else if (BlendModeStr == TEXT("Masked"))
			Material->BlendMode = BLEND_Masked;
		else if (BlendModeStr == TEXT("Translucent"))
			Material->BlendMode = BLEND_Translucent;
		else if (BlendModeStr == TEXT("Additive"))
			Material->BlendMode = BLEND_Additive;
		else if (BlendModeStr == TEXT("Modulate"))
			Material->BlendMode = BLEND_Modulate;
	}

	if (bHasTwoSided)
	{
		Material->TwoSided = bTwoSided;
	}

	Material->PostEditChange();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	// Refresh asset cache
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AllMaterialAssets.Empty();
	ARM.Get().GetAssetsByClass(UMaterial::StaticClass()->GetClassPathName(), AllMaterialAssets, false);

	// Map domain back to string for response
	auto DomainToString = [](EMaterialDomain Domain) -> FString
	{
		switch (Domain)
		{
		case MD_Surface:        return TEXT("Surface");
		case MD_DeferredDecal:  return TEXT("DeferredDecal");
		case MD_LightFunction:  return TEXT("LightFunction");
		case MD_Volume:         return TEXT("Volume");
		case MD_PostProcess:    return TEXT("PostProcess");
		case MD_UI:             return TEXT("UI");
		default:                return TEXT("Surface");
		}
	};

	auto BlendModeToString = [](EBlendMode Mode) -> FString
	{
		switch (Mode)
		{
		case BLEND_Opaque:      return TEXT("Opaque");
		case BLEND_Masked:      return TEXT("Masked");
		case BLEND_Translucent: return TEXT("Translucent");
		case BLEND_Additive:    return TEXT("Additive");
		case BLEND_Modulate:    return TEXT("Modulate");
		default:                return TEXT("Opaque");
		}
	};

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created Material '%s' (saved: %s)"),
		*Name, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), Material->GetPathName());
	Result->SetStringField(TEXT("domain"), DomainToString(Material->MaterialDomain));
	Result->SetStringField(TEXT("blendMode"), BlendModeToString(Material->BlendMode));
	Result->SetBoolField(TEXT("twoSided"), Material->TwoSided != 0);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleSetMaterialProperty — set a top-level material property
// ============================================================

FString FBlueprintMCPServer::HandleSetMaterialProperty(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString Property = Json->GetStringField(TEXT("property"));

	if (MaterialName.IsEmpty() || Property.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, property"));
	}

	if (!Json->HasField(TEXT("value")))
	{
		return MakeErrorJson(TEXT("Missing required field: value"));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	FString OldValue;
	FString NewValue;

	// Helper lambdas for converting enum values to strings
	auto DomainToString = [](EMaterialDomain Domain) -> FString
	{
		switch (Domain)
		{
		case MD_Surface:        return TEXT("Surface");
		case MD_DeferredDecal:  return TEXT("DeferredDecal");
		case MD_LightFunction:  return TEXT("LightFunction");
		case MD_Volume:         return TEXT("Volume");
		case MD_PostProcess:    return TEXT("PostProcess");
		case MD_UI:             return TEXT("UI");
		default:                return TEXT("Unknown");
		}
	};

	auto BlendModeToString = [](EBlendMode Mode) -> FString
	{
		switch (Mode)
		{
		case BLEND_Opaque:      return TEXT("Opaque");
		case BLEND_Masked:      return TEXT("Masked");
		case BLEND_Translucent: return TEXT("Translucent");
		case BLEND_Additive:    return TEXT("Additive");
		case BLEND_Modulate:    return TEXT("Modulate");
		default:                return TEXT("Unknown");
		}
	};

	auto ShadingModelToString = [](EMaterialShadingModel Model) -> FString
	{
		switch (Model)
		{
		case MSM_Unlit:                return TEXT("Unlit");
		case MSM_DefaultLit:           return TEXT("DefaultLit");
		case MSM_Subsurface:           return TEXT("Subsurface");
		case MSM_PreintegratedSkin:    return TEXT("PreintegratedSkin");
		case MSM_ClearCoat:            return TEXT("ClearCoat");
		case MSM_SubsurfaceProfile:    return TEXT("SubsurfaceProfile");
		case MSM_TwoSidedFoliage:      return TEXT("TwoSidedFoliage");
		case MSM_Hair:                 return TEXT("Hair");
		case MSM_Cloth:                return TEXT("Cloth");
		case MSM_Eye:                  return TEXT("Eye");
		default:                       return TEXT("DefaultLit");
		}
	};

	if (Property == TEXT("domain"))
	{
		FString ValueStr = Json->GetStringField(TEXT("value"));
		OldValue = DomainToString(Material->MaterialDomain);

		EMaterialDomain NewDomain = Material->MaterialDomain;
		if (ValueStr == TEXT("Surface"))            NewDomain = MD_Surface;
		else if (ValueStr == TEXT("DeferredDecal")) NewDomain = MD_DeferredDecal;
		else if (ValueStr == TEXT("LightFunction")) NewDomain = MD_LightFunction;
		else if (ValueStr == TEXT("Volume"))         NewDomain = MD_Volume;
		else if (ValueStr == TEXT("PostProcess"))    NewDomain = MD_PostProcess;
		else if (ValueStr == TEXT("UI"))             NewDomain = MD_UI;
		else
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Invalid domain '%s'. Valid values: Surface, DeferredDecal, LightFunction, Volume, PostProcess, UI"),
				*ValueStr));
		}

		NewValue = ValueStr;

		if (!bDryRun)
		{
			Material->PreEditChange(nullptr);
			Material->MaterialDomain = NewDomain;
			Material->PostEditChange();
		}
	}
	else if (Property == TEXT("blendMode"))
	{
		FString ValueStr = Json->GetStringField(TEXT("value"));
		OldValue = BlendModeToString(Material->BlendMode);

		EBlendMode NewBlend = Material->BlendMode;
		if (ValueStr == TEXT("Opaque"))           NewBlend = BLEND_Opaque;
		else if (ValueStr == TEXT("Masked"))      NewBlend = BLEND_Masked;
		else if (ValueStr == TEXT("Translucent")) NewBlend = BLEND_Translucent;
		else if (ValueStr == TEXT("Additive"))    NewBlend = BLEND_Additive;
		else if (ValueStr == TEXT("Modulate"))    NewBlend = BLEND_Modulate;
		else
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Invalid blendMode '%s'. Valid values: Opaque, Masked, Translucent, Additive, Modulate"),
				*ValueStr));
		}

		NewValue = ValueStr;

		if (!bDryRun)
		{
			Material->PreEditChange(nullptr);
			Material->BlendMode = NewBlend;
			Material->PostEditChange();
		}
	}
	else if (Property == TEXT("twoSided"))
	{
		bool bValue = Json->GetBoolField(TEXT("value"));
		OldValue = Material->TwoSided ? TEXT("true") : TEXT("false");
		NewValue = bValue ? TEXT("true") : TEXT("false");

		if (!bDryRun)
		{
			Material->PreEditChange(nullptr);
			Material->TwoSided = bValue ? 1 : 0;
			Material->PostEditChange();
		}
	}
	else if (Property == TEXT("shadingModel"))
	{
		FString ValueStr = Json->GetStringField(TEXT("value"));
		OldValue = ShadingModelToString(Material->GetShadingModels().GetFirstShadingModel());

		EMaterialShadingModel NewModel = MSM_DefaultLit;
		if (ValueStr == TEXT("Unlit"))                  NewModel = MSM_Unlit;
		else if (ValueStr == TEXT("DefaultLit"))        NewModel = MSM_DefaultLit;
		else if (ValueStr == TEXT("Subsurface"))        NewModel = MSM_Subsurface;
		else if (ValueStr == TEXT("PreintegratedSkin")) NewModel = MSM_PreintegratedSkin;
		else if (ValueStr == TEXT("ClearCoat"))         NewModel = MSM_ClearCoat;
		else if (ValueStr == TEXT("SubsurfaceProfile")) NewModel = MSM_SubsurfaceProfile;
		else if (ValueStr == TEXT("TwoSidedFoliage"))   NewModel = MSM_TwoSidedFoliage;
		else if (ValueStr == TEXT("Hair"))              NewModel = MSM_Hair;
		else if (ValueStr == TEXT("Cloth"))             NewModel = MSM_Cloth;
		else if (ValueStr == TEXT("Eye"))               NewModel = MSM_Eye;
		else
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Invalid shadingModel '%s'. Valid values: Unlit, DefaultLit, Subsurface, PreintegratedSkin, ClearCoat, SubsurfaceProfile, TwoSidedFoliage, Hair, Cloth, Eye"),
				*ValueStr));
		}

		NewValue = ValueStr;

		if (!bDryRun)
		{
			Material->PreEditChange(nullptr);
			Material->SetShadingModel(NewModel);
			Material->PostEditChange();
		}
	}
	else if (Property == TEXT("opacity"))
	{
		double OpacityValue = Json->GetNumberField(TEXT("value"));
		OldValue = FString::Printf(TEXT("%f"), Material->OpacityMaskClipValue);
		NewValue = FString::Printf(TEXT("%f"), OpacityValue);

		if (!bDryRun)
		{
			Material->PreEditChange(nullptr);
			Material->OpacityMaskClipValue = (float)OpacityValue;
			Material->PostEditChange();
		}
	}
	else
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Unknown property '%s'. Valid properties: domain, blendMode, twoSided, shadingModel, opacity"),
			*Property));
	}

	// Save if not dry run
	bool bSaved = false;
	if (!bDryRun)
	{
		bSaved = SaveMaterialPackage(Material);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: %sSet material property '%s' on '%s': '%s' -> '%s'"),
		bDryRun ? TEXT("[DRY RUN] ") : TEXT(""),
		*Property, *MaterialName, *OldValue, *NewValue);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("property"), Property);
	Result->SetStringField(TEXT("oldValue"), OldValue);
	Result->SetStringField(TEXT("newValue"), NewValue);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	if (!bDryRun)
	{
		Result->SetBoolField(TEXT("saved"), bSaved);
	}
	return JsonToString(Result);
}

// ============================================================
// HandleAddMaterialExpression — add a new expression to a material
// ============================================================

FString FBlueprintMCPServer::HandleAddMaterialExpression(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString ExpressionClassName = Json->GetStringField(TEXT("expressionClass"));

	if (MaterialName.IsEmpty() || ExpressionClassName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, expressionClass"));
	}

	int32 PosX = 0, PosY = 0;
	if (Json->HasField(TEXT("posX")))
		PosX = (int32)Json->GetNumberField(TEXT("posX"));
	if (Json->HasField(TEXT("posY")))
		PosY = (int32)Json->GetNumberField(TEXT("posY"));

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Map string class name to UClass
	UClass* ExprClass = nullptr;

	if (ExpressionClassName == TEXT("Constant"))
		ExprClass = UMaterialExpressionConstant::StaticClass();
	else if (ExpressionClassName == TEXT("Constant3Vector"))
		ExprClass = UMaterialExpressionConstant3Vector::StaticClass();
	else if (ExpressionClassName == TEXT("Constant4Vector"))
		ExprClass = UMaterialExpressionConstant4Vector::StaticClass();
	else if (ExpressionClassName == TEXT("ScalarParameter"))
		ExprClass = UMaterialExpressionScalarParameter::StaticClass();
	else if (ExpressionClassName == TEXT("VectorParameter"))
		ExprClass = UMaterialExpressionVectorParameter::StaticClass();
	else if (ExpressionClassName == TEXT("TextureSample"))
		ExprClass = UMaterialExpressionTextureSample::StaticClass();
	else if (ExpressionClassName == TEXT("TextureSampleParameter2D"))
		ExprClass = UMaterialExpressionTextureSampleParameter2D::StaticClass();
	else if (ExpressionClassName == TEXT("TextureCoordinate"))
		ExprClass = UMaterialExpressionTextureCoordinate::StaticClass();
	else if (ExpressionClassName == TEXT("ComponentMask"))
		ExprClass = UMaterialExpressionComponentMask::StaticClass();
	else if (ExpressionClassName == TEXT("Add"))
		ExprClass = UMaterialExpressionAdd::StaticClass();
	else if (ExpressionClassName == TEXT("Multiply"))
		ExprClass = UMaterialExpressionMultiply::StaticClass();
	else if (ExpressionClassName == TEXT("LinearInterpolate") || ExpressionClassName == TEXT("Lerp"))
		ExprClass = UMaterialExpressionLinearInterpolate::StaticClass();
	else if (ExpressionClassName == TEXT("Clamp"))
		ExprClass = UMaterialExpressionClamp::StaticClass();
	else if (ExpressionClassName == TEXT("OneMinus"))
		ExprClass = UMaterialExpressionOneMinus::StaticClass();
	else if (ExpressionClassName == TEXT("Power"))
		ExprClass = UMaterialExpressionPower::StaticClass();
	else if (ExpressionClassName == TEXT("Time"))
		ExprClass = UMaterialExpressionTime::StaticClass();
	else if (ExpressionClassName == TEXT("WorldPosition"))
		ExprClass = UMaterialExpressionWorldPosition::StaticClass();
	else if (ExpressionClassName == TEXT("AppendVector"))
		ExprClass = UMaterialExpressionAppendVector::StaticClass();
	else if (ExpressionClassName == TEXT("Custom"))
		ExprClass = UMaterialExpressionCustom::StaticClass();
	else if (ExpressionClassName == TEXT("StaticSwitchParameter"))
		ExprClass = UMaterialExpressionStaticSwitchParameter::StaticClass();
	else if (ExpressionClassName == TEXT("MaterialFunctionCall"))
		ExprClass = UMaterialExpressionMaterialFunctionCall::StaticClass();
	else
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Unknown expression class '%s'. Valid classes: Constant, Constant3Vector, Constant4Vector, "
				"ScalarParameter, VectorParameter, TextureSample, TextureSampleParameter2D, TextureCoordinate, "
				"ComponentMask, Add, Multiply, LinearInterpolate, Lerp, Clamp, OneMinus, Power, Time, "
				"WorldPosition, AppendVector, Custom, StaticSwitchParameter, MaterialFunctionCall"),
			*ExpressionClassName));
	}

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (bDryRun)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: [DRY RUN] Would add expression '%s' to material '%s' at (%d, %d)"),
			*ExpressionClassName, *MaterialName, PosX, PosY);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("expressionClass"), ExpressionClassName);
		Result->SetNumberField(TEXT("posX"), PosX);
		Result->SetNumberField(TEXT("posY"), PosY);
		return JsonToString(Result);
	}

	// Create the expression
	UMaterialExpression* NewExpr = NewObject<UMaterialExpression>(Material, ExprClass);
	if (!NewExpr)
	{
		return MakeErrorJson(TEXT("Failed to create material expression object"));
	}

	// Set position
	NewExpr->MaterialExpressionEditorX = PosX;
	NewExpr->MaterialExpressionEditorY = PosY;

	// Add to material (UE 5.4 compatible)
	Material->GetExpressionCollection().AddExpression(NewExpr);

	// Rebuild material graph if it exists
	if (Material->MaterialGraph)
	{
		Material->MaterialGraph->RebuildGraph();
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	// Find the node GUID from the material graph
	FString NodeGuid;
	if (Material->MaterialGraph)
	{
		for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
		{
			UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(Node);
			if (MatNode && MatNode->MaterialExpression == NewExpr)
			{
				NodeGuid = Node->NodeGuid.ToString();
				break;
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added expression '%s' to material '%s' (nodeId: %s, saved: %s)"),
		*ExpressionClassName, *MaterialName, *NodeGuid, bSaved ? TEXT("true") : TEXT("false"));

	// Serialize the expression details
	TSharedPtr<FJsonObject> ExprDetails = SerializeMaterialExpression(NewExpr);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("expressionClass"), ExpressionClassName);
	Result->SetStringField(TEXT("nodeId"), NodeGuid);
	Result->SetNumberField(TEXT("posX"), PosX);
	Result->SetNumberField(TEXT("posY"), PosY);
	if (ExprDetails.IsValid())
	{
		Result->SetObjectField(TEXT("expression"), ExprDetails);
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleDeleteMaterialExpression — remove an expression from a material
// ============================================================

FString FBlueprintMCPServer::HandleDeleteMaterialExpression(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));

	if (MaterialName.IsEmpty() || NodeId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, nodeId"));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	// Find the node by GUID
	UMaterialGraphNode* TargetMatNode = nullptr;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (!Node) continue;
		if (Node->NodeGuid.ToString() == NodeId)
		{
			TargetMatNode = Cast<UMaterialGraphNode>(Node);
			break;
		}
	}

	if (!TargetMatNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found in material graph"), *NodeId));
	}

	if (!TargetMatNode->MaterialExpression)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' has no associated material expression"), *NodeId));
	}

	// Capture info before deletion
	FString DeletedNodeTitle = TargetMatNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	FString DeletedExprClass = TargetMatNode->MaterialExpression->GetClass()->GetName();

	if (bDryRun)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: [DRY RUN] Would delete expression '%s' (nodeId: %s) from material '%s'"),
			*DeletedExprClass, *NodeId, *MaterialName);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("deletedNode"), NodeId);
		Result->SetStringField(TEXT("deletedNodeTitle"), DeletedNodeTitle);
		Result->SetStringField(TEXT("deletedExpressionClass"), DeletedExprClass);
		return JsonToString(Result);
	}

	// Remove the expression from the material
	UMaterialExpression* ExprToRemove = TargetMatNode->MaterialExpression;
	Material->GetExpressionCollection().RemoveExpression(ExprToRemove);
	ExprToRemove->MarkAsGarbage();

	// Rebuild graph
	Material->MaterialGraph->RebuildGraph();

	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	Material->MarkPackageDirty();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deleted expression '%s' (nodeId: %s) from material '%s' (saved: %s)"),
		*DeletedExprClass, *NodeId, *MaterialName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("deletedNode"), NodeId);
	Result->SetStringField(TEXT("deletedNodeTitle"), DeletedNodeTitle);
	Result->SetStringField(TEXT("deletedExpressionClass"), DeletedExprClass);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleConnectMaterialPins — connect two pins in a material graph
// ============================================================

FString FBlueprintMCPServer::HandleConnectMaterialPins(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString SourceNodeId = Json->GetStringField(TEXT("sourceNodeId"));
	FString SourcePinName = Json->GetStringField(TEXT("sourcePinName"));
	FString TargetNodeId = Json->GetStringField(TEXT("targetNodeId"));
	FString TargetPinName = Json->GetStringField(TEXT("targetPinName"));

	if (MaterialName.IsEmpty() || SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() ||
		TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, sourceNodeId, sourcePinName, targetNodeId, targetPinName"));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	// Find source and target nodes by GUID
	UEdGraphNode* SourceNode = nullptr;
	UEdGraphNode* TargetNode = nullptr;

	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (!Node) continue;
		if (Node->NodeGuid.ToString() == SourceNodeId)
			SourceNode = Node;
		if (Node->NodeGuid.ToString() == TargetNodeId)
			TargetNode = Node;
		if (SourceNode && TargetNode)
			break;
	}

	if (!SourceNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Source node '%s' not found in material graph"), *SourceNodeId));
	}
	if (!TargetNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Target node '%s' not found in material graph"), *TargetNodeId));
	}

	// Find pins
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

	UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
	if (!TargetPin)
	{
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

	if (bDryRun)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: [DRY RUN] Would connect %s.%s -> %s.%s in material '%s'"),
			*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName, *MaterialName);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetBoolField(TEXT("connected"), false);
		Result->SetStringField(TEXT("material"), Material->GetName());
		return JsonToString(Result);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Connecting %s.%s -> %s.%s in material '%s'"),
		*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName, *MaterialName);

	// Try to connect via the schema
	const UEdGraphSchema* Schema = Material->MaterialGraph->GetSchema();
	if (!Schema)
	{
		return MakeErrorJson(TEXT("Material graph schema not found"));
	}

	bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bConnected);
	Result->SetBoolField(TEXT("connected"), bConnected);
	Result->SetStringField(TEXT("material"), Material->GetName());

	if (!bConnected)
	{
		Result->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Cannot connect %s.%s to %s.%s — types may be incompatible"),
			*SourceNodeId, *SourcePinName, *TargetNodeId, *TargetPinName));
		return JsonToString(Result);
	}

	// Save
	Material->PreEditChange(nullptr);
	Material->PostEditChange();
	bool bSaved = SaveMaterialPackage(Material);
	Result->SetBoolField(TEXT("saved"), bSaved);

	return JsonToString(Result);
}

// ============================================================
// HandleDisconnectMaterialPin — break connections on a pin in a material graph
// ============================================================

FString FBlueprintMCPServer::HandleDisconnectMaterialPin(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));
	FString PinName = Json->GetStringField(TEXT("pinName"));

	if (MaterialName.IsEmpty() || NodeId.IsEmpty() || PinName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, nodeId, pinName"));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	// Find node by GUID
	UEdGraphNode* TargetNode = nullptr;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (!Node) continue;
		if (Node->NodeGuid.ToString() == NodeId)
		{
			TargetNode = Node;
			break;
		}
	}

	if (!TargetNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found in material graph"), *NodeId));
	}

	// Find pin
	UEdGraphPin* Pin = TargetNode->FindPin(FName(*PinName));
	if (!Pin)
	{
		TArray<TSharedPtr<FJsonValue>> PinNames;
		for (UEdGraphPin* P : TargetNode->Pins)
		{
			if (P) PinNames.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("%s (%s)"), *P->PinName.ToString(),
					P->Direction == EGPD_Input ? TEXT("Input") : TEXT("Output"))));
		}
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin '%s' not found on node '%s'"),
			*PinName, *NodeId));
		E->SetArrayField(TEXT("availablePins"), PinNames);
		return JsonToString(E);
	}

	int32 BrokenCount = Pin->LinkedTo.Num();

	if (bDryRun)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: [DRY RUN] Would disconnect pin '%s' on node '%s' in material '%s' (%d links)"),
			*PinName, *NodeId, *MaterialName, BrokenCount);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetStringField(TEXT("pinName"), PinName);
		Result->SetNumberField(TEXT("brokenLinkCount"), BrokenCount);
		return JsonToString(Result);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Disconnecting pin '%s' on node '%s' in material '%s' (%d links)"),
		*PinName, *NodeId, *MaterialName, BrokenCount);

	// Break all links
	Pin->BreakAllPinLinks();

	Material->PreEditChange(nullptr);
	Material->PostEditChange();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("pinName"), PinName);
	Result->SetNumberField(TEXT("brokenLinkCount"), BrokenCount);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleSetExpressionValue — set value on a material expression
// ============================================================

FString FBlueprintMCPServer::HandleSetExpressionValue(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));

	if (MaterialName.IsEmpty() || NodeId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, nodeId"));
	}

	if (!Json->HasField(TEXT("value")))
	{
		return MakeErrorJson(TEXT("Missing required field: value"));
	}

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	// Find the node by GUID
	UMaterialGraphNode* TargetMatNode = nullptr;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (!Node) continue;
		if (Node->NodeGuid.ToString() == NodeId)
		{
			TargetMatNode = Cast<UMaterialGraphNode>(Node);
			break;
		}
	}

	if (!TargetMatNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found in material graph"), *NodeId));
	}

	UMaterialExpression* Expr = TargetMatNode->MaterialExpression;
	if (!Expr)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' has no associated material expression"), *NodeId));
	}

	FString ExprType;
	FString NewValueStr;

	Material->PreEditChange(nullptr);

	// Handle based on expression type
	if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(Expr))
	{
		ExprType = TEXT("Constant");
		double Value = Json->GetNumberField(TEXT("value"));
		ConstExpr->R = (float)Value;
		NewValueStr = FString::Printf(TEXT("%f"), Value);
	}
	else if (UMaterialExpressionConstant3Vector* C3Expr = Cast<UMaterialExpressionConstant3Vector>(Expr))
	{
		ExprType = TEXT("Constant3Vector");
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Json->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
		{
			double R = 0, G = 0, B = 0;
			(*ValueObj)->TryGetNumberField(TEXT("r"), R);
			(*ValueObj)->TryGetNumberField(TEXT("g"), G);
			(*ValueObj)->TryGetNumberField(TEXT("b"), B);
			C3Expr->Constant = FLinearColor((float)R, (float)G, (float)B);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f)"), R, G, B);
		}
		else
		{
			Material->PostEditChange();
			return MakeErrorJson(TEXT("Constant3Vector requires value as object {r, g, b}"));
		}
	}
	else if (UMaterialExpressionConstant4Vector* C4Expr = Cast<UMaterialExpressionConstant4Vector>(Expr))
	{
		ExprType = TEXT("Constant4Vector");
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Json->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
		{
			double R = 0, G = 0, B = 0, A = 1;
			(*ValueObj)->TryGetNumberField(TEXT("r"), R);
			(*ValueObj)->TryGetNumberField(TEXT("g"), G);
			(*ValueObj)->TryGetNumberField(TEXT("b"), B);
			(*ValueObj)->TryGetNumberField(TEXT("a"), A);
			C4Expr->Constant = FLinearColor((float)R, (float)G, (float)B, (float)A);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f, %f)"), R, G, B, A);
		}
		else
		{
			Material->PostEditChange();
			return MakeErrorJson(TEXT("Constant4Vector requires value as object {r, g, b, a}"));
		}
	}
	else if (UMaterialExpressionScalarParameter* SPExpr = Cast<UMaterialExpressionScalarParameter>(Expr))
	{
		ExprType = TEXT("ScalarParameter");
		double Value = Json->GetNumberField(TEXT("value"));
		SPExpr->DefaultValue = (float)Value;
		NewValueStr = FString::Printf(TEXT("%f"), Value);

		FString ParamName;
		if (Json->TryGetStringField(TEXT("parameterName"), ParamName) && !ParamName.IsEmpty())
		{
			SPExpr->ParameterName = FName(*ParamName);
		}
	}
	else if (UMaterialExpressionVectorParameter* VPExpr = Cast<UMaterialExpressionVectorParameter>(Expr))
	{
		ExprType = TEXT("VectorParameter");
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Json->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
		{
			double R = 0, G = 0, B = 0, A = 1;
			(*ValueObj)->TryGetNumberField(TEXT("r"), R);
			(*ValueObj)->TryGetNumberField(TEXT("g"), G);
			(*ValueObj)->TryGetNumberField(TEXT("b"), B);
			(*ValueObj)->TryGetNumberField(TEXT("a"), A);
			VPExpr->DefaultValue = FLinearColor((float)R, (float)G, (float)B, (float)A);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f, %f)"), R, G, B, A);
		}
		else
		{
			Material->PostEditChange();
			return MakeErrorJson(TEXT("VectorParameter requires value as object {r, g, b, a}"));
		}

		FString ParamName;
		if (Json->TryGetStringField(TEXT("parameterName"), ParamName) && !ParamName.IsEmpty())
		{
			VPExpr->ParameterName = FName(*ParamName);
		}
	}
	else if (UMaterialExpressionTextureCoordinate* TCExpr = Cast<UMaterialExpressionTextureCoordinate>(Expr))
	{
		ExprType = TEXT("TextureCoordinate");
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Json->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
		{
			double CoordIndex = 0, UTiling = 1, VTiling = 1;
			(*ValueObj)->TryGetNumberField(TEXT("coordinateIndex"), CoordIndex);
			(*ValueObj)->TryGetNumberField(TEXT("uTiling"), UTiling);
			(*ValueObj)->TryGetNumberField(TEXT("vTiling"), VTiling);
			TCExpr->CoordinateIndex = (int32)CoordIndex;
			TCExpr->UTiling = (float)UTiling;
			TCExpr->VTiling = (float)VTiling;
			NewValueStr = FString::Printf(TEXT("(index=%d, uTiling=%f, vTiling=%f)"), (int32)CoordIndex, UTiling, VTiling);
		}
		else
		{
			Material->PostEditChange();
			return MakeErrorJson(TEXT("TextureCoordinate requires value as object {coordinateIndex, uTiling, vTiling}"));
		}
	}
	else if (UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expr))
	{
		ExprType = TEXT("Custom");
		FString Code;
		if (Json->TryGetStringField(TEXT("code"), Code))
		{
			CustomExpr->Code = Code;
			NewValueStr = FString::Printf(TEXT("Code: %d chars"), Code.Len());
		}
		else if (Json->HasField(TEXT("value")))
		{
			// Also accept code via value field as string
			FString ValueStr = Json->GetStringField(TEXT("value"));
			if (!ValueStr.IsEmpty())
			{
				CustomExpr->Code = ValueStr;
				NewValueStr = FString::Printf(TEXT("Code: %d chars"), ValueStr.Len());
			}
		}

		FString OutputTypeStr;
		if (Json->TryGetStringField(TEXT("outputType"), OutputTypeStr) && !OutputTypeStr.IsEmpty())
		{
			int64 EnumVal = StaticEnum<ECustomMaterialOutputType>()->GetValueByNameString(OutputTypeStr);
			if (EnumVal != INDEX_NONE)
			{
				CustomExpr->OutputType = (ECustomMaterialOutputType)EnumVal;
			}
		}
	}
	else if (UMaterialExpressionComponentMask* CMExpr = Cast<UMaterialExpressionComponentMask>(Expr))
	{
		ExprType = TEXT("ComponentMask");
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Json->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
		{
			bool bR = false, bG = false, bB = false, bA = false;
			(*ValueObj)->TryGetBoolField(TEXT("r"), bR);
			(*ValueObj)->TryGetBoolField(TEXT("g"), bG);
			(*ValueObj)->TryGetBoolField(TEXT("b"), bB);
			(*ValueObj)->TryGetBoolField(TEXT("a"), bA);
			CMExpr->R = bR ? 1 : 0;
			CMExpr->G = bG ? 1 : 0;
			CMExpr->B = bB ? 1 : 0;
			CMExpr->A = bA ? 1 : 0;
			NewValueStr = FString::Printf(TEXT("(R=%s, G=%s, B=%s, A=%s)"),
				bR ? TEXT("true") : TEXT("false"),
				bG ? TEXT("true") : TEXT("false"),
				bB ? TEXT("true") : TEXT("false"),
				bA ? TEXT("true") : TEXT("false"));
		}
		else
		{
			Material->PostEditChange();
			return MakeErrorJson(TEXT("ComponentMask requires value as object {r, g, b, a} (booleans)"));
		}
	}
	else
	{
		Material->PostEditChange();
		return MakeErrorJson(FString::Printf(
			TEXT("Expression type '%s' does not support direct value setting. Supported types: Constant, "
				"Constant3Vector, Constant4Vector, ScalarParameter, VectorParameter, TextureCoordinate, "
				"Custom, ComponentMask"),
			*Expr->GetClass()->GetName()));
	}

	Material->PostEditChange();
	Material->MarkPackageDirty();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Set expression value on node '%s' (%s) in material '%s': %s"),
		*NodeId, *ExprType, *MaterialName, *NewValueStr);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetStringField(TEXT("expressionType"), ExprType);
	Result->SetStringField(TEXT("newValue"), NewValueStr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleMoveMaterialExpression — reposition a material graph node
// ============================================================

FString FBlueprintMCPServer::HandleMoveMaterialExpression(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString NodeId = Json->GetStringField(TEXT("nodeId"));

	if (MaterialName.IsEmpty() || NodeId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, nodeId"));
	}

	if (!Json->HasField(TEXT("posX")) || !Json->HasField(TEXT("posY")))
	{
		return MakeErrorJson(TEXT("Missing required fields: posX, posY"));
	}

	int32 PosX = (int32)Json->GetNumberField(TEXT("posX"));
	int32 PosY = (int32)Json->GetNumberField(TEXT("posY"));

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	// Find node by GUID
	UMaterialGraphNode* TargetMatNode = nullptr;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (!Node) continue;
		if (Node->NodeGuid.ToString() == NodeId)
		{
			TargetMatNode = Cast<UMaterialGraphNode>(Node);
			break;
		}
	}

	if (!TargetMatNode)
	{
		return MakeErrorJson(FString::Printf(TEXT("Node '%s' not found in material graph"), *NodeId));
	}

	if (bDryRun)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: [DRY RUN] Would move node '%s' to (%d, %d) in material '%s'"),
			*NodeId, PosX, PosY, *MaterialName);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("dryRun"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetNumberField(TEXT("posX"), PosX);
		Result->SetNumberField(TEXT("posY"), PosY);
		return JsonToString(Result);
	}

	// Set position on the graph node
	TargetMatNode->NodePosX = PosX;
	TargetMatNode->NodePosY = PosY;

	// Also update the underlying expression position
	if (TargetMatNode->MaterialExpression)
	{
		TargetMatNode->MaterialExpression->MaterialExpressionEditorX = PosX;
		TargetMatNode->MaterialExpression->MaterialExpressionEditorY = PosY;
	}

	Material->PreEditChange(nullptr);
	Material->PostEditChange();

	// Save
	bool bSaved = SaveMaterialPackage(Material);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Moved node '%s' to (%d, %d) in material '%s' (saved: %s)"),
		*NodeId, PosX, PosY, *MaterialName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("nodeId"), NodeId);
	Result->SetNumberField(TEXT("posX"), PosX);
	Result->SetNumberField(TEXT("posY"), PosY);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// Phase 4: Create Material Function
// ============================================================

// ============================================================
// HandleCreateMaterialFunction — create a new UMaterialFunction asset
// ============================================================

FString FBlueprintMCPServer::HandleCreateMaterialFunction(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString Name = Json->GetStringField(TEXT("name"));
	FString PackagePath = Json->GetStringField(TEXT("packagePath"));

	if (Name.IsEmpty() || PackagePath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: name, packagePath"));
	}

	if (!PackagePath.StartsWith(TEXT("/Game")))
	{
		return MakeErrorJson(TEXT("packagePath must start with '/Game'"));
	}

	// Check if asset already exists
	FString FullAssetPath = PackagePath / Name;
	if (FindMaterialFunctionAsset(Name) || FindMaterialFunctionAsset(FullAssetPath))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Material Function '%s' already exists. Use a different name or delete the existing asset first."),
			*Name));
	}

	FString Description;
	Json->TryGetStringField(TEXT("description"), Description);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating Material Function '%s' in '%s'"), *Name, *PackagePath);

	// Create via IAssetTools + factory
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterialFunction::StaticClass(), Factory);

	if (!NewAsset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to create Material Function '%s' in '%s'"), *Name, *PackagePath));
	}

	UMaterialFunction* MF = Cast<UMaterialFunction>(NewAsset);
	if (!MF)
	{
		return MakeErrorJson(TEXT("Created asset is not a UMaterialFunction"));
	}

	// Set optional description
	if (!Description.IsEmpty())
	{
		MF->Description = Description;
	}

	// Save
	bool bSaved = SaveGenericPackage(MF);

	// Refresh asset cache
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	AllMaterialFunctionAssets.Empty();
	ARM.Get().GetAssetsByClass(UMaterialFunction::StaticClass()->GetClassPathName(), AllMaterialFunctionAssets, false);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created Material Function '%s' (saved: %s)"),
		*Name, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("path"), MF->GetPathName());
	if (!Description.IsEmpty())
	{
		Result->SetStringField(TEXT("description"), Description);
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// Phase 5: Material Snapshot/Diff/Restore
// ============================================================

// ============================================================
// HandleSnapshotMaterialGraph — snapshot current material graph state
// ============================================================

FString FBlueprintMCPServer::HandleSnapshotMaterialGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	if (MaterialName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: material"));
	}

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating snapshot for material '%s'"), *MaterialName);

	// Build the snapshot
	FGraphSnapshot Snapshot;
	Snapshot.SnapshotId = GenerateSnapshotId(MaterialName);
	Snapshot.BlueprintName = Material->GetName();
	Snapshot.BlueprintPath = Material->GetPathName();
	Snapshot.CreatedAt = FDateTime::Now();

	// Capture the material graph
	FGraphSnapshotData GraphData = CaptureGraphSnapshot(Material->MaterialGraph);

	int32 NodeCount = GraphData.Nodes.Num();
	int32 ConnectionCount = GraphData.Connections.Num();

	Snapshot.Graphs.Add(TEXT("MaterialGraph"), MoveTemp(GraphData));

	// Store in material snapshots (separate from blueprint snapshots)
	MaterialSnapshots.Add(Snapshot.SnapshotId, Snapshot);

	// Prune old material snapshots
	while (MaterialSnapshots.Num() > MaxSnapshots)
	{
		FString OldestId;
		FDateTime OldestTime = FDateTime::MaxValue();

		for (const auto& Pair : MaterialSnapshots)
		{
			if (Pair.Value.CreatedAt < OldestTime)
			{
				OldestTime = Pair.Value.CreatedAt;
				OldestId = Pair.Key;
			}
		}

		if (!OldestId.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Pruning old material snapshot '%s'"), *OldestId);
			MaterialSnapshots.Remove(OldestId);
		}
		else
		{
			break;
		}
	}

	// Save to disk
	SaveSnapshotToDisk(Snapshot.SnapshotId, Snapshot);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Material snapshot '%s' created with %d nodes, %d connections"),
		*Snapshot.SnapshotId, NodeCount, ConnectionCount);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetNumberField(TEXT("nodeCount"), NodeCount);
	Result->SetNumberField(TEXT("connectionCount"), ConnectionCount);
	return JsonToString(Result);
}

// ============================================================
// HandleDiffMaterialGraph — diff current material graph against snapshot
// ============================================================

FString FBlueprintMCPServer::HandleDiffMaterialGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));

	if (MaterialName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, snapshotId"));
	}

	// Load snapshot from material snapshots (memory or disk)
	FGraphSnapshot* SnapshotPtr = MaterialSnapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Diffing material '%s' against snapshot '%s'"), *MaterialName, *SnapshotId);

	// Capture current state
	FGraphSnapshotData CurrentData = CaptureGraphSnapshot(Material->MaterialGraph);

	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	TArray<TSharedPtr<FJsonValue>> SeveredArr;
	TArray<TSharedPtr<FJsonValue>> NewConnsArr;
	TArray<TSharedPtr<FJsonValue>> MissingNodesArr;

	// Process the MaterialGraph from the snapshot
	const FGraphSnapshotData* SnapDataPtr = SnapshotPtr->Graphs.Find(TEXT("MaterialGraph"));
	if (!SnapDataPtr)
	{
		return MakeErrorJson(TEXT("Snapshot does not contain a MaterialGraph"));
	}

	const FGraphSnapshotData& SnapData = *SnapDataPtr;

	// Build node lookup maps
	TMap<FString, const FNodeRecord*> SnapNodeLookup;
	for (const FNodeRecord& NR : SnapData.Nodes)
	{
		SnapNodeLookup.Add(NR.NodeGuid, &NR);
	}

	TMap<FString, const FNodeRecord*> CurNodeLookup;
	for (const FNodeRecord& NR : CurrentData.Nodes)
	{
		CurNodeLookup.Add(NR.NodeGuid, &NR);
	}

	// Build connection sets
	TSet<FString> SnapConnSet;
	for (const FPinConnectionRecord& Conn : SnapData.Connections)
	{
		SnapConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
	}

	TSet<FString> CurrentConnSet;
	for (const FPinConnectionRecord& Conn : CurrentData.Connections)
	{
		CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
	}

	// Find severed connections: in snapshot but not in current
	for (const FPinConnectionRecord& Conn : SnapData.Connections)
	{
		FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
		if (!CurrentConnSet.Contains(Key))
		{
			TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
			SJ->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
			SJ->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
			SJ->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);
			SJ->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);

			const FNodeRecord** SrcRec = SnapNodeLookup.Find(Conn.SourceNodeGuid);
			if (SrcRec) SJ->SetStringField(TEXT("sourceNodeName"), (*SrcRec)->NodeTitle);
			const FNodeRecord** TgtRec = SnapNodeLookup.Find(Conn.TargetNodeGuid);
			if (TgtRec) SJ->SetStringField(TEXT("targetNodeName"), (*TgtRec)->NodeTitle);

			SeveredArr.Add(MakeShared<FJsonValueObject>(SJ));
		}
	}

	// Find new connections: in current but not in snapshot
	for (const FPinConnectionRecord& Conn : CurrentData.Connections)
	{
		FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
		if (!SnapConnSet.Contains(Key))
		{
			TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
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

	// Find missing nodes: in snapshot but not in current
	for (const FNodeRecord& SnapNode : SnapData.Nodes)
	{
		const FNodeRecord** CurNodePtr = CurNodeLookup.Find(SnapNode.NodeGuid);
		if (!CurNodePtr)
		{
			TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
			MJ->SetStringField(TEXT("nodeGuid"), SnapNode.NodeGuid);
			MJ->SetStringField(TEXT("nodeClass"), SnapNode.NodeClass);
			MJ->SetStringField(TEXT("nodeTitle"), SnapNode.NodeTitle);
			MissingNodesArr.Add(MakeShared<FJsonValueObject>(MJ));
		}
	}

	// Build result
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("snapshotId"), SnapshotId);
	Result->SetArrayField(TEXT("severedConnections"), SeveredArr);
	Result->SetArrayField(TEXT("newConnections"), NewConnsArr);
	Result->SetArrayField(TEXT("missingNodes"), MissingNodesArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("severedConnections"), SeveredArr.Num());
	Summary->SetNumberField(TEXT("newConnections"), NewConnsArr.Num());
	Summary->SetNumberField(TEXT("missingNodes"), MissingNodesArr.Num());
	Result->SetObjectField(TEXT("summary"), Summary);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Material diff complete — %d severed, %d new, %d missing nodes"),
		SeveredArr.Num(), NewConnsArr.Num(), MissingNodesArr.Num());

	return JsonToString(Result);
}

// ============================================================
// HandleRestoreMaterialGraph — restore material graph connections from snapshot
// ============================================================

FString FBlueprintMCPServer::HandleRestoreMaterialGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString MaterialName = Json->GetStringField(TEXT("material"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));

	if (MaterialName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: material, snapshotId"));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load snapshot from material snapshots (memory or disk)
	FGraphSnapshot* SnapshotPtr = MaterialSnapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load material
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' has no material graph"), *MaterialName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Restoring material connections from snapshot '%s' for material '%s' (dryRun=%s)"),
		*SnapshotId, *MaterialName, bDryRun ? TEXT("true") : TEXT("false"));

	// Capture current state for comparison
	FGraphSnapshotData CurrentData = CaptureGraphSnapshot(Material->MaterialGraph);

	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	// Build current connection set
	TSet<FString> CurrentConnSet;
	for (const FPinConnectionRecord& Conn : CurrentData.Connections)
	{
		CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
	}

	// Build node lookup for the material graph
	TMap<FString, UEdGraphNode*> NodeLookup;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		if (Node)
		{
			NodeLookup.Add(Node->NodeGuid.ToString(), Node);
		}
	}

	const FGraphSnapshotData* SnapDataPtr = SnapshotPtr->Graphs.Find(TEXT("MaterialGraph"));
	if (!SnapDataPtr)
	{
		return MakeErrorJson(TEXT("Snapshot does not contain a MaterialGraph"));
	}

	int32 Reconnected = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> DetailsArr;

	for (const FPinConnectionRecord& Conn : SnapDataPtr->Connections)
	{
		FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
		if (CurrentConnSet.Contains(Key)) continue; // Still connected, skip

		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
		Detail->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);
		Detail->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
		Detail->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);

		// Find source and target nodes
		UEdGraphNode** SourceNodePtr = NodeLookup.Find(Conn.SourceNodeGuid);
		UEdGraphNode** TargetNodePtr = NodeLookup.Find(Conn.TargetNodeGuid);

		if (!SourceNodePtr || !*SourceNodePtr)
		{
			Detail->SetStringField(TEXT("result"), TEXT("failed"));
			Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Source node '%s' no longer exists"), *Conn.SourceNodeGuid));
			Failed++;
			DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
			continue;
		}
		if (!TargetNodePtr || !*TargetNodePtr)
		{
			Detail->SetStringField(TEXT("result"), TEXT("failed"));
			Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Target node '%s' no longer exists"), *Conn.TargetNodeGuid));
			Failed++;
			DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
			continue;
		}

		UEdGraphNode* SourceNode = *SourceNodePtr;
		UEdGraphNode* TargetNode = *TargetNodePtr;

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
			Detail->SetStringField(TEXT("result"), TEXT("would_reconnect"));
			Reconnected++;
			DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
			continue;
		}

		// Try to reconnect via the schema
		const UEdGraphSchema* Schema = Material->MaterialGraph->GetSchema();
		if (!Schema)
		{
			Detail->SetStringField(TEXT("result"), TEXT("failed"));
			Detail->SetStringField(TEXT("reason"), TEXT("Material graph schema not found"));
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
			Detail->SetStringField(TEXT("reason"), TEXT("TryCreateConnection failed — types may be incompatible"));
			Failed++;
		}
		DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
	}

	// Save if not dry run and we reconnected something
	bool bSaved = false;
	if (!bDryRun && Reconnected > 0)
	{
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		bSaved = SaveMaterialPackage(Material);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Material restore complete — %d reconnected, %d failed, saved=%s"),
		Reconnected, Failed, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("snapshotId"), SnapshotId);
	Result->SetNumberField(TEXT("reconnected"), Reconnected);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("details"), DetailsArr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	return JsonToString(Result);
}
