#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EditablePinBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

// ============================================================
// Request handlers
// ============================================================

FString FBlueprintMCPServer::HandleList(const TMap<FString, FString>& Params)
{
	const FString* Filter = Params.Find(TEXT("filter"));
	const FString* ParentClassFilter = Params.Find(TEXT("parentClass"));
	const FString* TypeFilter = Params.Find(TEXT("type"));
	// type: "all" (default), "regular", "level"
	bool bIncludeRegular = !TypeFilter || TypeFilter->IsEmpty() || *TypeFilter == TEXT("all") || *TypeFilter == TEXT("regular");
	bool bIncludeLevel = !TypeFilter || TypeFilter->IsEmpty() || *TypeFilter == TEXT("all") || *TypeFilter == TEXT("level");

	TArray<TSharedPtr<FJsonValue>> Entries;
	if (bIncludeRegular)
	for (const FAssetData& Asset : AllBlueprintAssets)
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

		FString ParentClass;
		Asset.GetTagValue(FName(TEXT("ParentClass")), ParentClass);
		// Tag stores full path — extract short name
		int32 DotIndex;
		if (ParentClass.FindLastChar('.', DotIndex))
		{
			ParentClass = ParentClass.Mid(DotIndex + 1);
		}

		if (ParentClassFilter && !ParentClassFilter->IsEmpty())
		{
			if (!ParentClass.Contains(*ParentClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("parentClass"), ParentClass);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Also include level blueprints from maps
	if (bIncludeLevel)
	for (const FAssetData& Asset : AllMapAssets)
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

		// No parent class filter for level blueprints
		if (ParentClassFilter && !ParentClassFilter->IsEmpty())
		{
			if (!FString(TEXT("LevelScriptActor")).Contains(*ParentClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("path"), Path);
		Entry->SetStringField(TEXT("parentClass"), TEXT("LevelScriptActor"));
		Entry->SetBoolField(TEXT("isLevelBlueprint"), true);
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Entries.Num());
	Result->SetNumberField(TEXT("total"), AllBlueprintAssets.Num() + AllMapAssets.Num());
	Result->SetArrayField(TEXT("blueprints"), Entries);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleGetBlueprint(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' parameter"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	return JsonToString(SerializeBlueprint(BP));
}

FString FBlueprintMCPServer::HandleGetGraph(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	const FString* GraphName = Params.Find(TEXT("graph"));
	if (!Name || Name->IsEmpty() || !GraphName || GraphName->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' or 'graph' parameter"));
	}

	// URL-decode graph name to handle spaces encoded as '+' or '%20'
	FString DecodedGraphName = UrlDecode(*GraphName);

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(DecodedGraphName, ESearchCase::IgnoreCase))
		{
			TSharedPtr<FJsonObject> GraphJson = SerializeGraph(Graph);
			if (GraphJson.IsValid())
			{
				return JsonToString(GraphJson.ToSharedRef());
			}
		}
	}

	// Not found — list available graphs
	TArray<TSharedPtr<FJsonValue>> GraphNames;
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph)
		{
			GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		}
	}
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph '%s' not found"), *DecodedGraphName));
	E->SetArrayField(TEXT("availableGraphs"), GraphNames);
	return JsonToString(E);
}

FString FBlueprintMCPServer::HandleSearch(const TMap<FString, FString>& Params)
{
	const FString* Query = Params.Find(TEXT("query"));
	if (!Query || Query->IsEmpty())
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("error"), TEXT("Missing 'query' parameter"));
		return JsonToString(E);
	}

	const FString* PathFilter = Params.Find(TEXT("path"));

	int32 MaxResults = 50;
	if (const FString* M = Params.Find(TEXT("maxResults")))
	{
		MaxResults = FMath::Clamp(FCString::Atoi(**M), 1, 200);
	}

	// Build a combined list of all searchable blueprints (regular + level)
	auto SearchBlueprint = [&](const FString& AssetName, const FString& Path, UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutResults)
	{
		TArray<UEdGraph*> Graphs;
		BP->GetAllGraphs(Graphs);

		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph || OutResults.Num() >= MaxResults) break;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || OutResults.Num() >= MaxResults) break;

				FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

				FString FuncName, EventName, VarName;
				if (auto* CF = Cast<UK2Node_CallFunction>(Node))
				{
					FuncName = CF->FunctionReference.GetMemberName().ToString();
				}
				else if (auto* Ev = Cast<UK2Node_Event>(Node))
				{
					EventName = Ev->EventReference.GetMemberName().ToString();
				}
				else if (auto* CE = Cast<UK2Node_CustomEvent>(Node))
				{
					EventName = CE->CustomFunctionName.ToString();
				}
				else if (auto* VG = Cast<UK2Node_VariableGet>(Node))
				{
					VarName = VG->GetVarName().ToString();
				}
				else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
				{
					VarName = VS->GetVarName().ToString();
				}

				bool bMatch = Title.Contains(*Query, ESearchCase::IgnoreCase) ||
					(!FuncName.IsEmpty() && FuncName.Contains(*Query, ESearchCase::IgnoreCase)) ||
					(!EventName.IsEmpty() && EventName.Contains(*Query, ESearchCase::IgnoreCase)) ||
					(!VarName.IsEmpty() && VarName.Contains(*Query, ESearchCase::IgnoreCase));

				if (bMatch)
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("blueprint"), AssetName);
					R->SetStringField(TEXT("blueprintPath"), Path);
					R->SetStringField(TEXT("graph"), Graph->GetName());
					R->SetStringField(TEXT("nodeTitle"), Title);
					R->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
					if (!FuncName.IsEmpty()) R->SetStringField(TEXT("functionName"), FuncName);
					if (!EventName.IsEmpty()) R->SetStringField(TEXT("eventName"), EventName);
					if (!VarName.IsEmpty()) R->SetStringField(TEXT("variableName"), VarName);
					OutResults.Add(MakeShared<FJsonValueObject>(R));
				}
			}
		}
	};

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = Asset.PackageName.ToString();
		if (PathFilter && !PathFilter->IsEmpty() && !Path.Contains(*PathFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
		if (!BP) continue;

		SearchBlueprint(Asset.AssetName.ToString(), Path, BP, Results);
	}

	// Also search level blueprints
	for (FAssetData& MapAsset : AllMapAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = MapAsset.PackageName.ToString();
		if (PathFilter && !PathFilter->IsEmpty() && !Path.Contains(*PathFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UWorld* World = Cast<UWorld>(MapAsset.GetAsset());
		if (!World || !World->PersistentLevel) continue;
		ULevelScriptBlueprint* LevelBP = World->PersistentLevel->GetLevelScriptBlueprint(false);
		if (!LevelBP) continue;

		int32 BeforeCount = Results.Num();
		SearchBlueprint(MapAsset.AssetName.ToString(), Path, LevelBP, Results);
		// Tag newly-added entries as level blueprint results
		for (int32 i = BeforeCount; i < Results.Num(); ++i)
		{
			Results[i]->AsObject()->SetBoolField(TEXT("isLevelBlueprint"), true);
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), *Query);
	Result->SetNumberField(TEXT("resultCount"), Results.Num());
	Result->SetArrayField(TEXT("results"), Results);
	return JsonToString(Result);
}

// ============================================================
// HandleTestSave — load a Blueprint and save it unmodified (diagnostic)
// ============================================================

FString FBlueprintMCPServer::HandleTestSave(const TMap<FString, FString>& Params)
{
	const FString* Name = Params.Find(TEXT("name"));
	if (!Name || Name->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'name' query parameter"));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: test-save requested for '%s'"), **Name);

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(*Name, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: test-save — loaded '%s', GeneratedClass=%s"),
		*BP->GetName(),
		BP->GeneratedClass ? *BP->GeneratedClass->GetName() : TEXT("null"));

	// Attempt save with NO modifications
	bool bSaved = SaveBlueprintPackage(BP);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), *Name);
	Result->SetStringField(TEXT("packagePath"), BP->GetPackage()->GetName());
	Result->SetBoolField(TEXT("hasGeneratedClass"), BP->GeneratedClass != nullptr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleFindReferences — find all Blueprints referencing an asset
// ============================================================

FString FBlueprintMCPServer::HandleFindReferences(const TMap<FString, FString>& Params)
{
	const FString* AssetPath = Params.Find(TEXT("assetPath"));
	if (!AssetPath || AssetPath->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'assetPath' query parameter"));
	}

	IAssetRegistry& Registry = *IAssetRegistry::Get();

	TArray<FName> Referencers;
	Registry.GetReferencers(FName(**AssetPath), Referencers);

	// Build set of known Blueprint package names for filtering
	TSet<FString> BlueprintPackages;
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		BlueprintPackages.Add(Asset.PackageName.ToString());
	}

	TArray<TSharedPtr<FJsonValue>> BPRefs;
	TArray<TSharedPtr<FJsonValue>> OtherRefs;
	for (const FName& Ref : Referencers)
	{
		FString RefStr = Ref.ToString();
		if (BlueprintPackages.Contains(RefStr))
		{
			BPRefs.Add(MakeShared<FJsonValueString>(RefStr));
		}
		else
		{
			OtherRefs.Add(MakeShared<FJsonValueString>(RefStr));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetPath"), **AssetPath);
	Result->SetNumberField(TEXT("totalReferencers"), Referencers.Num());
	Result->SetNumberField(TEXT("blueprintReferencerCount"), BPRefs.Num());
	Result->SetArrayField(TEXT("blueprintReferencers"), BPRefs);
	Result->SetNumberField(TEXT("otherReferencerCount"), OtherRefs.Num());
	Result->SetArrayField(TEXT("otherReferencers"), OtherRefs);
	return JsonToString(Result);
}

// ============================================================
// HandleSearchByType — find all usages of a type across blueprints
// ============================================================

FString FBlueprintMCPServer::HandleSearchByType(const TMap<FString, FString>& Params)
{
	const FString* TypeNamePtr = Params.Find(TEXT("typeName"));
	if (!TypeNamePtr || TypeNamePtr->IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'typeName' query parameter"));
	}

	FString TypeName = UrlDecode(*TypeNamePtr);
	const FString* Filter = Params.Find(TEXT("filter"));
	FString FilterStr = Filter ? UrlDecode(*Filter) : FString();

	int32 MaxResults = 200;
	if (const FString* M = Params.Find(TEXT("maxResults")))
	{
		MaxResults = FMath::Clamp(FCString::Atoi(**M), 1, 500);
	}

	// Strip F/E/U prefix for comparison
	FString TypeNameNoPrefix = TypeName;
	if (TypeNameNoPrefix.StartsWith(TEXT("F")) || TypeNameNoPrefix.StartsWith(TEXT("E")) || TypeNameNoPrefix.StartsWith(TEXT("U")))
	{
		TypeNameNoPrefix = TypeNameNoPrefix.Mid(1);
	}

	auto MatchesType = [&TypeName, &TypeNameNoPrefix](const FString& TestType) -> bool
	{
		return TestType.Equals(TypeName, ESearchCase::IgnoreCase) ||
			TestType.Equals(TypeNameNoPrefix, ESearchCase::IgnoreCase);
	};

	TArray<TSharedPtr<FJsonValue>> Results;

	// Lambda that searches a single Blueprint for type usages
	auto SearchOneBlueprint = [&](const FString& BPName, const FString& Path, UBlueprint* BP, bool bIsLevel)
	{
		// Check variables
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Results.Num() >= MaxResults) break;

			FString VarSubtype;
			if (Var.VarType.PinSubCategoryObject.IsValid())
			{
				VarSubtype = Var.VarType.PinSubCategoryObject->GetName();
			}

			if (MatchesType(VarSubtype) || MatchesType(Var.VarType.PinCategory.ToString()))
			{
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetStringField(TEXT("blueprint"), BPName);
				R->SetStringField(TEXT("blueprintPath"), Path);
				R->SetStringField(TEXT("usage"), TEXT("variable"));
				R->SetStringField(TEXT("location"), Var.VarName.ToString());
				R->SetStringField(TEXT("currentType"), Var.VarType.PinCategory.ToString());
				if (!VarSubtype.IsEmpty())
					R->SetStringField(TEXT("currentSubtype"), VarSubtype);
				if (bIsLevel)
					R->SetBoolField(TEXT("isLevelBlueprint"), true);
				Results.Add(MakeShared<FJsonValueObject>(R));
			}
		}

		// Check graphs for function/event params, struct nodes, and pin connections
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph || Results.Num() >= MaxResults) break;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || Results.Num() >= MaxResults) break;

				// Check FunctionEntry/CustomEvent parameters
				if (auto* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
				{
					for (const TSharedPtr<FUserPinInfo>& PinInfo : FuncEntry->UserDefinedPins)
					{
						if (!PinInfo.IsValid()) continue;
						FString ParamSubtype;
						if (PinInfo->PinType.PinSubCategoryObject.IsValid())
							ParamSubtype = PinInfo->PinType.PinSubCategoryObject->GetName();

						if (MatchesType(ParamSubtype) || MatchesType(PinInfo->PinType.PinCategory.ToString()))
						{
							TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
							R->SetStringField(TEXT("blueprint"), BPName);
							R->SetStringField(TEXT("blueprintPath"), Path);
							R->SetStringField(TEXT("usage"), TEXT("functionParameter"));
							R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
								*Graph->GetName(), *PinInfo->PinName.ToString()));
							R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							R->SetStringField(TEXT("currentType"), PinInfo->PinType.PinCategory.ToString());
							if (!ParamSubtype.IsEmpty())
								R->SetStringField(TEXT("currentSubtype"), ParamSubtype);
							if (bIsLevel)
								R->SetBoolField(TEXT("isLevelBlueprint"), true);
							Results.Add(MakeShared<FJsonValueObject>(R));
						}
					}
				}
				else if (auto* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					for (const TSharedPtr<FUserPinInfo>& PinInfo : CustomEvent->UserDefinedPins)
					{
						if (!PinInfo.IsValid()) continue;
						FString ParamSubtype;
						if (PinInfo->PinType.PinSubCategoryObject.IsValid())
							ParamSubtype = PinInfo->PinType.PinSubCategoryObject->GetName();

						if (MatchesType(ParamSubtype) || MatchesType(PinInfo->PinType.PinCategory.ToString()))
						{
							TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
							R->SetStringField(TEXT("blueprint"), BPName);
							R->SetStringField(TEXT("blueprintPath"), Path);
							R->SetStringField(TEXT("usage"), TEXT("eventParameter"));
							R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
								*CustomEvent->CustomFunctionName.ToString(), *PinInfo->PinName.ToString()));
							R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							R->SetStringField(TEXT("currentType"), PinInfo->PinType.PinCategory.ToString());
							if (!ParamSubtype.IsEmpty())
								R->SetStringField(TEXT("currentSubtype"), ParamSubtype);
							if (bIsLevel)
								R->SetBoolField(TEXT("isLevelBlueprint"), true);
							Results.Add(MakeShared<FJsonValueObject>(R));
						}
					}
				}
				// Check Break/Make struct nodes
				else if (auto* BreakNode = Cast<UK2Node_BreakStruct>(Node))
				{
					if (BreakNode->StructType && MatchesType(BreakNode->StructType->GetName()))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("breakStruct"));
						R->SetStringField(TEXT("location"), Graph->GetName());
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("structType"), BreakNode->StructType->GetName());
						if (bIsLevel)
							R->SetBoolField(TEXT("isLevelBlueprint"), true);
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}
				else if (auto* MakeNode = Cast<UK2Node_MakeStruct>(Node))
				{
					if (MakeNode->StructType && MatchesType(MakeNode->StructType->GetName()))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("makeStruct"));
						R->SetStringField(TEXT("location"), Graph->GetName());
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("structType"), MakeNode->StructType->GetName());
						if (bIsLevel)
							R->SetBoolField(TEXT("isLevelBlueprint"), true);
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}

				// Check pin connections carrying the type
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->bHidden || Results.Num() >= MaxResults) continue;

					FString PinSubtype;
					if (Pin->PinType.PinSubCategoryObject.IsValid())
						PinSubtype = Pin->PinType.PinSubCategoryObject->GetName();

					if (Pin->LinkedTo.Num() > 0 &&
						(MatchesType(PinSubtype) || MatchesType(Pin->PinType.PinCategory.ToString())))
					{
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), BPName);
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("usage"), TEXT("pinConnection"));
						R->SetStringField(TEXT("location"), FString::Printf(TEXT("%s.%s"),
							*Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString(),
							*Pin->PinName.ToString()));
						R->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						R->SetStringField(TEXT("graph"), Graph->GetName());
						R->SetStringField(TEXT("pinType"), Pin->PinType.PinCategory.ToString());
						if (!PinSubtype.IsEmpty())
							R->SetStringField(TEXT("pinSubtype"), PinSubtype);
						R->SetNumberField(TEXT("connectionCount"), Pin->LinkedTo.Num());
						if (bIsLevel)
							R->SetBoolField(TEXT("isLevelBlueprint"), true);
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}
			}
		}
	};

	// Search regular blueprints
	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = Asset.PackageName.ToString();
		FString BPName = Asset.AssetName.ToString();

		if (!FilterStr.IsEmpty() && !BPName.Contains(FilterStr, ESearchCase::IgnoreCase) &&
			!Path.Contains(FilterStr, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
		if (!BP) continue;

		SearchOneBlueprint(BPName, Path, BP, false);
	}

	// Search level blueprints from maps
	for (FAssetData& MapAsset : AllMapAssets)
	{
		if (Results.Num() >= MaxResults) break;

		FString Path = MapAsset.PackageName.ToString();
		FString MapName = MapAsset.AssetName.ToString();

		if (!FilterStr.IsEmpty() && !MapName.Contains(FilterStr, ESearchCase::IgnoreCase) &&
			!Path.Contains(FilterStr, ESearchCase::IgnoreCase))
		{
			continue;
		}

		UWorld* World = Cast<UWorld>(MapAsset.GetAsset());
		if (!World || !World->PersistentLevel) continue;
		ULevelScriptBlueprint* LevelBP = World->PersistentLevel->GetLevelScriptBlueprint(false);
		if (!LevelBP) continue;

		SearchOneBlueprint(MapName, Path, LevelBP, true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("typeName"), TypeName);
	Result->SetNumberField(TEXT("resultCount"), Results.Num());
	Result->SetArrayField(TEXT("results"), Results);
	return JsonToString(Result);
}
