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
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

// ============================================================
// HandleListMaterials — list Material and MaterialInstance assets
// ============================================================

FString FBlueprintMCPServer::HandleListMaterials(const TMap<FString, FString>& Params)
{
	const FString* Filter = Params.Find(TEXT("filter"));
	const FString* TypeFilter = Params.Find(TEXT("type"));

	bool bIncludeMaterials = !TypeFilter || TypeFilter->IsEmpty() || *TypeFilter == TEXT("all") || *TypeFilter == TEXT("material");
	bool bIncludeInstances = !TypeFilter || TypeFilter->IsEmpty() || *TypeFilter == TEXT("all") || *TypeFilter == TEXT("instance");

	TArray<TSharedPtr<FJsonValue>> Entries;

	if (bIncludeMaterials)
	{
		for (const FAssetData& Asset : AllMaterialAssets)
		{
			FString Name = Asset.AssetName.ToString();
			FString Path = Asset.PackageName.ToString();

			if (Filter && !Filter->IsEmpty())
			{
				if (!Name.Contains(*Filter, ESearchCase::IgnoreCase) &&
					!Path.Contains(*Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			Entry->SetStringField(TEXT("path"), Path);
			Entry->SetStringField(TEXT("type"), TEXT("Material"));
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	if (bIncludeInstances)
	{
		for (const FAssetData& Asset : AllMaterialInstanceAssets)
		{
			FString Name = Asset.AssetName.ToString();
			FString Path = Asset.PackageName.ToString();

			if (Filter && !Filter->IsEmpty())
			{
				if (!Name.Contains(*Filter, ESearchCase::IgnoreCase) &&
					!Path.Contains(*Filter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			Entry->SetStringField(TEXT("path"), Path);
			Entry->SetStringField(TEXT("type"), TEXT("MaterialInstance"));
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	int32 Total = AllMaterialAssets.Num() + AllMaterialInstanceAssets.Num();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetArrayField(TEXT("materials"), Entries);
	return JsonToString(Result);
}

// ============================================================
// HandleGetMaterial — detailed info about a material or instance
// ============================================================

FString FBlueprintMCPServer::HandleGetMaterial(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' parameter"));
	}

	FString DecodedName = UrlDecode(*Name);

	// Try loading as UMaterial first
	FString LoadError;
	UMaterial* Material = LoadMaterialByName(DecodedName, LoadError);
	if (Material)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: GetMaterial — loaded material '%s'"), *Material->GetName());

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Material->GetName());
		Result->SetStringField(TEXT("path"), Material->GetPathName());
		Result->SetStringField(TEXT("type"), TEXT("Material"));

		// Material domain
		FString DomainStr = TEXT("Unknown");
		if (const UEnum* DomainEnum = StaticEnum<EMaterialDomain>())
		{
			DomainStr = DomainEnum->GetNameStringByValue((int64)Material->MaterialDomain);
		}
		Result->SetStringField(TEXT("domain"), DomainStr);

		// Blend mode
		FString BlendModeStr = TEXT("Unknown");
		if (const UEnum* BlendEnum = StaticEnum<EBlendMode>())
		{
			BlendModeStr = BlendEnum->GetNameStringByValue((int64)Material->BlendMode);
		}
		Result->SetStringField(TEXT("blendMode"), BlendModeStr);

		// Shading models
		TArray<TSharedPtr<FJsonValue>> ShadingModels;
		FMaterialShadingModelField SMField = Material->GetShadingModels();
		if (const UEnum* SMEnum = StaticEnum<EMaterialShadingModel>())
		{
			for (int32 i = 0; i < SMEnum->NumEnums() - 1; ++i)
			{
				EMaterialShadingModel SM = (EMaterialShadingModel)SMEnum->GetValueByIndex(i);
				if (SMField.HasShadingModel(SM))
				{
					ShadingModels.Add(MakeShared<FJsonValueString>(SMEnum->GetNameStringByIndex(i)));
				}
			}
		}
		Result->SetArrayField(TEXT("shadingModels"), ShadingModels);

		// Two-sided
		Result->SetBoolField(TEXT("twoSided"), Material->IsTwoSided());

		// Expression count
		auto Expressions = Material->GetExpressions();
		Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());

		// Parameters — iterate expressions for parameter types
		TArray<TSharedPtr<FJsonValue>> Parameters;
		for (UMaterialExpression* Expr : Expressions)
		{
			if (!Expr) continue;

			TSharedRef<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			bool bIsParam = false;

			if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
			{
				bIsParam = true;
				ParamObj->SetStringField(TEXT("name"), SP->ParameterName.ToString());
				ParamObj->SetStringField(TEXT("type"), TEXT("Scalar"));
				ParamObj->SetStringField(TEXT("group"), SP->Group.ToString());
				ParamObj->SetNumberField(TEXT("defaultValue"), SP->DefaultValue);
			}
			else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
			{
				bIsParam = true;
				ParamObj->SetStringField(TEXT("name"), VP->ParameterName.ToString());
				ParamObj->SetStringField(TEXT("type"), TEXT("Vector"));
				ParamObj->SetStringField(TEXT("group"), VP->Group.ToString());
				TSharedRef<FJsonObject> DefVal = MakeShared<FJsonObject>();
				DefVal->SetNumberField(TEXT("r"), VP->DefaultValue.R);
				DefVal->SetNumberField(TEXT("g"), VP->DefaultValue.G);
				DefVal->SetNumberField(TEXT("b"), VP->DefaultValue.B);
				DefVal->SetNumberField(TEXT("a"), VP->DefaultValue.A);
				ParamObj->SetObjectField(TEXT("defaultValue"), DefVal);
			}
			else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
			{
				bIsParam = true;
				ParamObj->SetStringField(TEXT("name"), TP->ParameterName.ToString());
				ParamObj->SetStringField(TEXT("type"), TEXT("Texture"));
				ParamObj->SetStringField(TEXT("group"), TP->Group.ToString());
				if (TP->Texture)
					ParamObj->SetStringField(TEXT("defaultValue"), TP->Texture->GetPathName());
			}
			else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
			{
				bIsParam = true;
				ParamObj->SetStringField(TEXT("name"), SSP->ParameterName.ToString());
				ParamObj->SetStringField(TEXT("type"), TEXT("StaticSwitch"));
				ParamObj->SetStringField(TEXT("group"), SSP->Group.ToString());
				ParamObj->SetBoolField(TEXT("defaultValue"), SSP->DefaultValue);
			}

			if (bIsParam)
			{
				Parameters.Add(MakeShared<FJsonValueObject>(ParamObj));
			}
		}
		Result->SetArrayField(TEXT("parameters"), Parameters);

		// Referenced textures
		TArray<TSharedPtr<FJsonValue>> ReferencedTextures;
		auto RefTexObjs = Material->GetReferencedTextures();
		for (const TObjectPtr<UObject>& TexObj : RefTexObjs)
		{
			if (TexObj)
			{
				ReferencedTextures.Add(MakeShared<FJsonValueString>(TexObj->GetPathName()));
			}
		}
		Result->SetArrayField(TEXT("referencedTextures"), ReferencedTextures);

		// Graph node count
		int32 GraphNodeCount = 0;
		if (Material->MaterialGraph)
		{
			GraphNodeCount = Material->MaterialGraph->Nodes.Num();
		}
		Result->SetNumberField(TEXT("graphNodeCount"), GraphNodeCount);

		return JsonToString(Result);
	}

	// Try loading as MaterialInstance
	FString MILoadError;
	UMaterialInstanceConstant* MI = LoadMaterialInstanceByName(DecodedName, MILoadError);
	if (MI)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: GetMaterial — loaded material instance '%s'"), *MI->GetName());

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), MI->GetName());
		Result->SetStringField(TEXT("path"), MI->GetPathName());
		Result->SetStringField(TEXT("type"), TEXT("MaterialInstance"));

		if (MI->Parent)
		{
			Result->SetStringField(TEXT("parent"), MI->Parent->GetName());
			Result->SetStringField(TEXT("parentPath"), MI->Parent->GetPathName());
		}

		// Overridden parameters
		TArray<TSharedPtr<FJsonValue>> OverriddenParams;

		// Scalar parameters
		for (const FScalarParameterValue& Param : MI->ScalarParameterValues)
		{
			TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			PObj->SetStringField(TEXT("type"), TEXT("Scalar"));
			PObj->SetNumberField(TEXT("value"), Param.ParameterValue);
			OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
		}

		// Vector parameters
		for (const FVectorParameterValue& Param : MI->VectorParameterValues)
		{
			TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			PObj->SetStringField(TEXT("type"), TEXT("Vector"));
			TSharedRef<FJsonObject> Val = MakeShared<FJsonObject>();
			Val->SetNumberField(TEXT("r"), Param.ParameterValue.R);
			Val->SetNumberField(TEXT("g"), Param.ParameterValue.G);
			Val->SetNumberField(TEXT("b"), Param.ParameterValue.B);
			Val->SetNumberField(TEXT("a"), Param.ParameterValue.A);
			PObj->SetObjectField(TEXT("value"), Val);
			OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
		}

		// Texture parameters
		for (const FTextureParameterValue& Param : MI->TextureParameterValues)
		{
			TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			PObj->SetStringField(TEXT("type"), TEXT("Texture"));
			if (Param.ParameterValue)
				PObj->SetStringField(TEXT("value"), Param.ParameterValue->GetPathName());
			else
				PObj->SetStringField(TEXT("value"), TEXT("None"));
			OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
		}

		// Static switch parameters
		for (const FStaticSwitchParameter& Param : MI->GetStaticParameters().StaticSwitchParameters)
		{
			TSharedRef<FJsonObject> PObj = MakeShared<FJsonObject>();
			PObj->SetStringField(TEXT("name"), Param.ParameterInfo.Name.ToString());
			PObj->SetStringField(TEXT("type"), TEXT("StaticSwitch"));
			PObj->SetBoolField(TEXT("value"), Param.Value);
			PObj->SetBoolField(TEXT("overridden"), Param.bOverride);
			OverriddenParams.Add(MakeShared<FJsonValueObject>(PObj));
		}

		Result->SetArrayField(TEXT("overriddenParameters"), OverriddenParams);

		return JsonToString(Result);
	}

	return MakeErrorJson(FString::Printf(TEXT("Material or MaterialInstance '%s' not found. Use list_materials to see available assets."), *DecodedName));
}

// ============================================================
// HandleGetMaterialGraph — serialized graph for a material
// ============================================================

FString FBlueprintMCPServer::HandleGetMaterialGraph(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' parameter"));
	}

	FString DecodedName = UrlDecode(*Name);

	FString LoadError;
	UMaterial* Material = LoadMaterialByName(DecodedName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: GetMaterialGraph — material '%s'"), *Material->GetName());

	// Ensure the material graph is built
	if (!Material->MaterialGraph)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: GetMaterialGraph — MaterialGraph is null, attempting rebuild"));
		// The material graph is built lazily by the material editor; force-create it
		Material->MaterialGraph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(Material, NAME_None, UMaterialGraph::StaticClass(), UMaterialGraphSchema::StaticClass()));
		Material->MaterialGraph->Material = Material;
		Material->MaterialGraph->RebuildGraph();
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(TEXT("Could not build MaterialGraph for this material"));
	}

	TSharedPtr<FJsonObject> GraphJson = SerializeGraph(Material->MaterialGraph);
	if (!GraphJson.IsValid())
	{
		return MakeErrorJson(TEXT("Failed to serialize material graph"));
	}

	// Add material name context
	GraphJson->SetStringField(TEXT("material"), Material->GetName());
	GraphJson->SetStringField(TEXT("materialPath"), Material->GetPathName());

	return JsonToString(GraphJson.ToSharedRef());
}

// ============================================================
// HandleDescribeMaterial — human-readable material description
// ============================================================

FString FBlueprintMCPServer::HandleDescribeMaterial(const FString& Body)
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

	FString LoadError;
	UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
	if (!Material)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: DescribeMaterial — '%s'"), *Material->GetName());

	// Ensure material graph is built
	if (!Material->MaterialGraph)
	{
		Material->MaterialGraph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(Material, NAME_None, UMaterialGraph::StaticClass(), UMaterialGraphSchema::StaticClass()));
		Material->MaterialGraph->Material = Material;
		Material->MaterialGraph->RebuildGraph();
	}

	if (!Material->MaterialGraph)
	{
		return MakeErrorJson(TEXT("Could not build MaterialGraph for this material"));
	}

	// Recursive helper: trace backwards from a pin and build a description string
	TFunction<FString(UEdGraphPin*, int32)> TracePin = [&TracePin](UEdGraphPin* Pin, int32 Depth) -> FString
	{
		if (!Pin || Depth > 10)
			return TEXT("(unknown)");

		// If no connections, report as unconnected
		if (Pin->LinkedTo.Num() == 0)
		{
			if (!Pin->DefaultValue.IsEmpty())
				return FString::Printf(TEXT("(default: %s)"), *Pin->DefaultValue);
			return TEXT("(unconnected)");
		}

		TArray<FString> Sources;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;

			UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();
			FString NodeDesc;

			// Check if this is a material graph node
			if (UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(SourceNode))
			{
				UMaterialExpression* Expr = MatNode->MaterialExpression;
				if (!Expr)
				{
					NodeDesc = TEXT("(null expression)");
				}
				else if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("ScalarParam \"%s\" (default: %.4f)"), *SP->ParameterName.ToString(), SP->DefaultValue);
				}
				else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("VectorParam \"%s\" (default: R=%.2f G=%.2f B=%.2f A=%.2f)"),
						*VP->ParameterName.ToString(), VP->DefaultValue.R, VP->DefaultValue.G, VP->DefaultValue.B, VP->DefaultValue.A);
				}
				else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				{
					FString TexName = TP->Texture ? TP->Texture->GetName() : TEXT("None");
					NodeDesc = FString::Printf(TEXT("TextureParam \"%s\" (%s)"), *TP->ParameterName.ToString(), *TexName);
				}
				else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("StaticSwitchParam \"%s\" (default: %s)"),
						*SSP->ParameterName.ToString(), SSP->DefaultValue ? TEXT("true") : TEXT("false"));
				}
				else if (auto* SC = Cast<UMaterialExpressionConstant>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("Constant(%.4f)"), SC->R);
				}
				else if (auto* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("Constant3(R=%.2f G=%.2f B=%.2f)"), C3->Constant.R, C3->Constant.G, C3->Constant.B);
				}
				else if (auto* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
				{
					NodeDesc = FString::Printf(TEXT("Constant4(R=%.2f G=%.2f B=%.2f A=%.2f)"), C4->Constant.R, C4->Constant.G, C4->Constant.B, C4->Constant.A);
				}
				else if (auto* TS = Cast<UMaterialExpressionTextureSample>(Expr))
				{
					FString TexName = TS->Texture ? TS->Texture->GetName() : TEXT("None");
					NodeDesc = FString::Printf(TEXT("TextureSample(%s)"), *TexName);
				}
				else if (auto* MFC = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
				{
					FString FuncName = MFC->MaterialFunction ? MFC->MaterialFunction->GetName() : TEXT("None");
					NodeDesc = FString::Printf(TEXT("FunctionCall(%s)"), *FuncName);
				}
				else
				{
					NodeDesc = Expr->GetClass()->GetName();
				}

				// If the source node has input pins with connections, recurse
				TArray<FString> InputDescs;
				for (UEdGraphPin* InputPin : SourceNode->Pins)
				{
					if (!InputPin || InputPin->Direction != EGPD_Input || InputPin->LinkedTo.Num() == 0) continue;
					FString InputDesc = TracePin(InputPin, Depth + 1);
					InputDescs.Add(InputDesc);
				}

				if (InputDescs.Num() > 0)
				{
					NodeDesc += TEXT(" <- (") + FString::Join(InputDescs, TEXT(", ")) + TEXT(")");
				}
			}
			else
			{
				// Non-material node (e.g., root, comment), just use title
				NodeDesc = SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			}

			Sources.Add(NodeDesc);
		}

		if (Sources.Num() == 1)
			return Sources[0];

		return TEXT("(") + FString::Join(Sources, TEXT(", ")) + TEXT(")");
	};

	// Find root node and trace each input
	TArray<TSharedPtr<FJsonValue>> InputDescriptions;

	UMaterialGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
	{
		RootNode = Cast<UMaterialGraphNode_Root>(Node);
		if (RootNode) break;
	}

	if (!RootNode)
	{
		return MakeErrorJson(TEXT("Could not find root node in material graph"));
	}

	for (UEdGraphPin* Pin : RootNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Input) continue;

		FString PinName = Pin->PinName.ToString();
		FString Description;

		if (Pin->LinkedTo.Num() == 0)
		{
			Description = TEXT("(unconnected)");
		}
		else
		{
			Description = TracePin(Pin, 0);
		}

		TSharedRef<FJsonObject> InputObj = MakeShared<FJsonObject>();
		InputObj->SetStringField(TEXT("input"), PinName);
		InputObj->SetStringField(TEXT("chain"), Description);
		InputObj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);
		InputDescriptions.Add(MakeShared<FJsonValueObject>(InputObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), Material->GetName());
	Result->SetStringField(TEXT("materialPath"), Material->GetPathName());
	Result->SetArrayField(TEXT("inputs"), InputDescriptions);

	// Also include a compact text description
	FString TextDesc;
	for (const TSharedPtr<FJsonValue>& Val : InputDescriptions)
	{
		TSharedPtr<FJsonObject> Obj = Val->AsObject();
		if (!Obj.IsValid()) continue;
		FString InputName = Obj->GetStringField(TEXT("input"));
		FString Chain = Obj->GetStringField(TEXT("chain"));
		bool bConnected = Obj->GetBoolField(TEXT("connected"));
		if (bConnected)
		{
			TextDesc += FString::Printf(TEXT("%s <- %s\n"), *InputName, *Chain);
		}
	}
	if (!TextDesc.IsEmpty())
	{
		Result->SetStringField(TEXT("description"), TextDesc);
	}

	return JsonToString(Result);
}

// ============================================================
// HandleSearchMaterials — search expressions and parameters
// ============================================================

FString FBlueprintMCPServer::HandleSearchMaterials(const TMap<FString, FString>& Params)
{
	const FString* Query = Params.Find(TEXT("query"));
	if (!Query || Query->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'query' parameter"));
	}

	FString DecodedQuery = UrlDecode(*Query);

	int32 MaxResults = 50;
	if (const FString* M = Params.Find(TEXT("maxResults")))
	{
		MaxResults = FMath::Clamp(FCString::Atoi(**M), 1, 200);
	}

	TArray<TSharedPtr<FJsonValue>> Results;

	for (const FAssetData& Asset : AllMaterialAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString MatName = Asset.AssetName.ToString();

		// Check material name first
		bool bNameMatch = MatName.Contains(DecodedQuery, ESearchCase::IgnoreCase);

		UMaterial* Material = Cast<UMaterial>(const_cast<FAssetData&>(Asset).GetAsset());
		if (!Material) continue;

		auto Expressions = Material->GetExpressions();

		if (bNameMatch)
		{
			// Add a match for the material itself
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("material"), MatName);
			R->SetStringField(TEXT("materialPath"), Asset.PackageName.ToString());
			R->SetStringField(TEXT("matchType"), TEXT("materialName"));
			Results.Add(MakeShared<FJsonValueObject>(R));
		}

		// Search expressions
		for (UMaterialExpression* Expr : Expressions)
		{
			if (!Expr || Results.Num() >= MaxResults) continue;

			FString ExprDesc = Expr->GetDescription();
			FString ExprClass = Expr->GetClass()->GetName();

			// Check parameter name
			FString ParamName;
			if (auto* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
				ParamName = SP->ParameterName.ToString();
			else if (auto* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
				ParamName = VP->ParameterName.ToString();
			else if (auto* TP = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
				ParamName = TP->ParameterName.ToString();
			else if (auto* SSP = Cast<UMaterialExpressionStaticSwitchParameter>(Expr))
				ParamName = SSP->ParameterName.ToString();

			bool bExprMatch = ExprDesc.Contains(DecodedQuery, ESearchCase::IgnoreCase) ||
				ExprClass.Contains(DecodedQuery, ESearchCase::IgnoreCase) ||
				(!ParamName.IsEmpty() && ParamName.Contains(DecodedQuery, ESearchCase::IgnoreCase));

			if (bExprMatch)
			{
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("material"), MatName);
				R->SetStringField(TEXT("materialPath"), Asset.PackageName.ToString());
				R->SetStringField(TEXT("matchType"), TEXT("expression"));
				R->SetStringField(TEXT("expressionClass"), ExprClass);
				if (!ExprDesc.IsEmpty())
					R->SetStringField(TEXT("description"), ExprDesc);
				if (!ParamName.IsEmpty())
					R->SetStringField(TEXT("parameterName"), ParamName);
				Results.Add(MakeShared<FJsonValueObject>(R));
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), DecodedQuery);
	Result->SetNumberField(TEXT("resultCount"), Results.Num());
	Result->SetArrayField(TEXT("results"), Results);
	return JsonToString(Result);
}

// ============================================================
// HandleFindMaterialReferences — find assets referencing a material
// ============================================================

FString FBlueprintMCPServer::HandleFindMaterialReferences(const FString& Body)
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

	// Try to find the material's package path
	FString PackagePath;
	FAssetData* MatAsset = FindMaterialAsset(MaterialName);
	if (MatAsset)
	{
		PackagePath = MatAsset->PackageName.ToString();
	}
	else
	{
		// Try material instance
		FAssetData* MIAsset = FindMaterialInstanceAsset(MaterialName);
		if (MIAsset)
		{
			PackagePath = MIAsset->PackageName.ToString();
		}
	}

	if (PackagePath.IsEmpty())
	{
		return MakeErrorJson(FString::Printf(TEXT("Material '%s' not found. Use list_materials to see available assets."), *MaterialName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: FindMaterialReferences — '%s' (package: %s)"), *MaterialName, *PackagePath);

	IAssetRegistry& Registry = *IAssetRegistry::Get();

	TArray<FName> Referencers;
	Registry.GetReferencers(FName(*PackagePath), Referencers);

	TArray<TSharedPtr<FJsonValue>> RefArray;
	for (const FName& Ref : Referencers)
	{
		FString RefStr = Ref.ToString();
		// Skip self-reference
		if (RefStr == PackagePath) continue;
		RefArray.Add(MakeShared<FJsonValueString>(RefStr));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("material"), MaterialName);
	Result->SetStringField(TEXT("packagePath"), PackagePath);
	Result->SetNumberField(TEXT("referencerCount"), RefArray.Num());
	Result->SetArrayField(TEXT("referencers"), RefArray);
	return JsonToString(Result);
}

// ============================================================
// HandleListMaterialFunctions — list MaterialFunction assets
// ============================================================

FString FBlueprintMCPServer::HandleListMaterialFunctions(const TMap<FString, FString>& Params)
{
	const FString* Filter = Params.Find(TEXT("filter"));

	TArray<TSharedPtr<FJsonValue>> Entries;

	for (const FAssetData& Asset : AllMaterialFunctionAssets)
	{
		FString Name = Asset.AssetName.ToString();
		FString Path = Asset.PackageName.ToString();

		if (Filter && !Filter->IsEmpty())
		{
			if (!Name.Contains(*Filter, ESearchCase::IgnoreCase) &&
				!Path.Contains(*Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetNumberField(TEXT("total"), AllMaterialFunctionAssets.Num());
	Result->SetArrayField(TEXT("functions"), Entries);
	return JsonToString(Result);
}

// ============================================================
// HandleGetMaterialFunction — detailed info about a material function
// ============================================================

FString FBlueprintMCPServer::HandleGetMaterialFunction(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' parameter"));
	}

	FString DecodedName = UrlDecode(*Name);

	FString LoadError;
	UMaterialFunction* MF = LoadMaterialFunctionByName(DecodedName, LoadError);
	if (!MF)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: GetMaterialFunction — '%s'"), *MF->GetName());

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), MF->GetName());
	Result->SetStringField(TEXT("path"), MF->GetPathName());
	Result->SetStringField(TEXT("description"), MF->GetDescription());

	// Expression count
	auto Expressions = MF->GetExpressions();
	Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());

	// List function inputs and outputs from expressions
	TArray<TSharedPtr<FJsonValue>> Inputs;
	TArray<TSharedPtr<FJsonValue>> Outputs;
	TArray<TSharedPtr<FJsonValue>> ExpressionList;

	{
		for (UMaterialExpression* Expr : Expressions)
		{
			if (!Expr) continue;

			if (auto* FI = Cast<UMaterialExpressionFunctionInput>(Expr))
			{
				TSharedRef<FJsonObject> InputObj = MakeShared<FJsonObject>();
				InputObj->SetStringField(TEXT("name"), FI->InputName.ToString());
				InputObj->SetStringField(TEXT("type"), TEXT("FunctionInput"));
				InputObj->SetNumberField(TEXT("posX"), FI->MaterialExpressionEditorX);
				InputObj->SetNumberField(TEXT("posY"), FI->MaterialExpressionEditorY);
				Inputs.Add(MakeShared<FJsonValueObject>(InputObj));
			}
			else if (auto* FO = Cast<UMaterialExpressionFunctionOutput>(Expr))
			{
				TSharedRef<FJsonObject> OutputObj = MakeShared<FJsonObject>();
				OutputObj->SetStringField(TEXT("name"), FO->OutputName.ToString());
				OutputObj->SetStringField(TEXT("type"), TEXT("FunctionOutput"));
				OutputObj->SetNumberField(TEXT("posX"), FO->MaterialExpressionEditorX);
				OutputObj->SetNumberField(TEXT("posY"), FO->MaterialExpressionEditorY);
				Outputs.Add(MakeShared<FJsonValueObject>(OutputObj));
			}

			// Serialize every expression
			TSharedPtr<FJsonObject> ExprJson = SerializeMaterialExpression(Expr);
			if (ExprJson.IsValid())
			{
				ExpressionList.Add(MakeShared<FJsonValueObject>(ExprJson.ToSharedRef()));
			}
		}
	}

	Result->SetArrayField(TEXT("inputs"), Inputs);
	Result->SetArrayField(TEXT("outputs"), Outputs);
	Result->SetArrayField(TEXT("expressions"), ExpressionList);

	// If the function has an editor graph, serialize it
	UEdGraph* FuncGraph = MF->MaterialGraph;
	if (FuncGraph)
	{
		TSharedPtr<FJsonObject> GraphJson = SerializeGraph(FuncGraph);
		if (GraphJson.IsValid())
		{
			Result->SetObjectField(TEXT("graph"), GraphJson);
		}
	}

	return JsonToString(Result);
}
