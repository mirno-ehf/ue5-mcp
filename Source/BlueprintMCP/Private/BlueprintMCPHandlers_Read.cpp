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

// ============================================================
// Skeleton inspection
// ============================================================

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMeshSocket.h"

FString FBlueprintMCPServer::HandleGetSkeleton(const TMap<FString, FString>& Params)
{
	const FString* PathParam = Params.Find(TEXT("path"));
	const FString* NameParam = Params.Find(TEXT("name"));
	if ((!PathParam || PathParam->IsEmpty()) && (!NameParam || NameParam->IsEmpty()))
	{
		return MakeErrorJson(TEXT("Missing 'path' (preferred) or 'name' parameter. Example: path=/Game/Characters/CC/Backend/CC4/CC5_Rig"));
	}

	USkeleton* Skeleton = nullptr;
	FString ResolvedPath;

	if (PathParam && !PathParam->IsEmpty())
	{
		ResolvedPath = *PathParam;
		// Accept both "/Game/Foo/Bar" and "/Game/Foo/Bar.Bar"
		FString ObjectPath = ResolvedPath;
		if (!ObjectPath.Contains(TEXT(".")))
		{
			FString LeafName;
			ObjectPath.Split(TEXT("/"), nullptr, &LeafName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			ObjectPath = ResolvedPath + TEXT(".") + LeafName;
		}
		Skeleton = LoadObject<USkeleton>(nullptr, *ObjectPath);
	}
	else if (NameParam && !NameParam->IsEmpty())
	{
		ResolvedPath = *NameParam;
		// Asset registry lookup by short name
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> Found;
		AssetRegistryModule.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Found);
		for (const FAssetData& A : Found)
		{
			if (A.AssetName.ToString().Equals(*NameParam, ESearchCase::IgnoreCase))
			{
				Skeleton = Cast<USkeleton>(A.GetAsset());
				if (Skeleton) { ResolvedPath = A.GetObjectPathString(); break; }
			}
		}
	}

	if (!Skeleton)
	{
		return MakeErrorJson(FString::Printf(TEXT("Skeleton not found: %s"), *ResolvedPath));
	}

	const FReferenceSkeleton& RefSk = Skeleton->GetReferenceSkeleton();
	const TArray<FMeshBoneInfo>& BoneInfos = RefSk.GetRefBoneInfo();
	const TArray<FTransform>& BonePose = RefSk.GetRefBonePose();
	const int32 NumBones = BoneInfos.Num();

	TArray<TSharedPtr<FJsonValue>> Bones;
	Bones.Reserve(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		const FMeshBoneInfo& Info = BoneInfos[i];
		TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetNumberField(TEXT("index"), i);
		B->SetStringField(TEXT("name"), Info.Name.ToString());
		B->SetNumberField(TEXT("parentIndex"), Info.ParentIndex);
		B->SetStringField(TEXT("parentName"), Info.ParentIndex >= 0 ? BoneInfos[Info.ParentIndex].Name.ToString() : FString());

		if (BonePose.IsValidIndex(i))
		{
			const FTransform& T = BonePose[i];
			const FVector L = T.GetLocation();
			const FQuat Q = T.GetRotation();
			const FVector S = T.GetScale3D();
			B->SetNumberField(TEXT("locX"), L.X);
			B->SetNumberField(TEXT("locY"), L.Y);
			B->SetNumberField(TEXT("locZ"), L.Z);
			B->SetNumberField(TEXT("rotX"), Q.X);
			B->SetNumberField(TEXT("rotY"), Q.Y);
			B->SetNumberField(TEXT("rotZ"), Q.Z);
			B->SetNumberField(TEXT("rotW"), Q.W);
			B->SetNumberField(TEXT("scaleX"), S.X);
			B->SetNumberField(TEXT("scaleY"), S.Y);
			B->SetNumberField(TEXT("scaleZ"), S.Z);
		}
		Bones.Add(MakeShared<FJsonValueObject>(B));
	}

	// Sockets (with transforms)
	TArray<TSharedPtr<FJsonValue>> Sockets;
	for (USkeletalMeshSocket* Socket : Skeleton->Sockets)
	{
		if (!Socket) continue;
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("name"), Socket->SocketName.ToString());
		S->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
		const FVector L = Socket->RelativeLocation;
		const FRotator R = Socket->RelativeRotation;
		const FVector Sc = Socket->RelativeScale;
		S->SetNumberField(TEXT("locX"), L.X);
		S->SetNumberField(TEXT("locY"), L.Y);
		S->SetNumberField(TEXT("locZ"), L.Z);
		S->SetNumberField(TEXT("rotPitch"), R.Pitch);
		S->SetNumberField(TEXT("rotYaw"), R.Yaw);
		S->SetNumberField(TEXT("rotRoll"), R.Roll);
		S->SetNumberField(TEXT("scaleX"), Sc.X);
		S->SetNumberField(TEXT("scaleY"), Sc.Y);
		S->SetNumberField(TEXT("scaleZ"), Sc.Z);
		Sockets.Add(MakeShared<FJsonValueObject>(S));
	}

	// Curve names (animation curves stored on skeleton)
	TArray<TSharedPtr<FJsonValue>> CurveNames;
	{
		TArray<FName> Names;
		Skeleton->GetCurveMetaDataNames(Names);
		Names.Sort(FNameLexicalLess());
		for (const FName& N : Names)
		{
			CurveNames.Add(MakeShared<FJsonValueString>(N.ToString()));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Skeleton->GetName());
	Result->SetStringField(TEXT("path"), ResolvedPath);
	Result->SetNumberField(TEXT("boneCount"), NumBones);
	Result->SetArrayField(TEXT("bones"), Bones);
	Result->SetNumberField(TEXT("socketCount"), Sockets.Num());
	Result->SetArrayField(TEXT("sockets"), Sockets);
	Result->SetNumberField(TEXT("curveCount"), CurveNames.Num());
	Result->SetArrayField(TEXT("curves"), CurveNames);
	return JsonToString(Result);
}

// ============================================================
// Skeleton mutation: add / remove / copy sockets
// ============================================================

namespace
{
	// Resolve a USkeleton by 'path' (preferred) or 'name'. Returns nullptr on failure.
	USkeleton* ResolveSkeletonFromJson(const TSharedPtr<FJsonObject>& Json, FString& OutResolvedPath, FString& OutError)
	{
		FString PathField, NameField;
		Json->TryGetStringField(TEXT("path"), PathField);
		Json->TryGetStringField(TEXT("name"), NameField);
		if (PathField.IsEmpty() && NameField.IsEmpty())
		{
			OutError = TEXT("Missing 'path' or 'name'.");
			return nullptr;
		}

		USkeleton* Skeleton = nullptr;
		if (!PathField.IsEmpty())
		{
			OutResolvedPath = PathField;
			FString ObjectPath = PathField;
			if (!ObjectPath.Contains(TEXT(".")))
			{
				FString LeafName;
				ObjectPath.Split(TEXT("/"), nullptr, &LeafName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
				ObjectPath = PathField + TEXT(".") + LeafName;
			}
			Skeleton = LoadObject<USkeleton>(nullptr, *ObjectPath);
		}
		if (!Skeleton && !NameField.IsEmpty())
		{
			OutResolvedPath = NameField;
			FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			TArray<FAssetData> Found;
			AssetRegistryModule.Get().GetAssetsByClass(USkeleton::StaticClass()->GetClassPathName(), Found);
			for (const FAssetData& A : Found)
			{
				if (A.AssetName.ToString().Equals(NameField, ESearchCase::IgnoreCase))
				{
					Skeleton = Cast<USkeleton>(A.GetAsset());
					if (Skeleton) { OutResolvedPath = A.GetObjectPathString(); break; }
				}
			}
		}

		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("Skeleton not found: %s"), *OutResolvedPath);
		}
		return Skeleton;
	}

	// Find a socket by name on a skeleton (returns index or INDEX_NONE).
	int32 FindSocketIndex(USkeleton* Skeleton, FName SocketName)
	{
		for (int32 i = 0; i < Skeleton->Sockets.Num(); ++i)
		{
			if (Skeleton->Sockets[i] && Skeleton->Sockets[i]->SocketName == SocketName)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}
}

FString FBlueprintMCPServer::HandleAddSkeletonSocket(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString ResolvedPath, ResolveError;
	USkeleton* Skeleton = ResolveSkeletonFromJson(Json, ResolvedPath, ResolveError);
	if (!Skeleton)
	{
		return MakeErrorJson(ResolveError);
	}

	FString SocketName, BoneName;
	if (!Json->TryGetStringField(TEXT("socketName"), SocketName) || SocketName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'socketName'."));
	}
	if (!Json->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'bone'."));
	}

	// Validate bone exists on this skeleton
	const FReferenceSkeleton& RefSk = Skeleton->GetReferenceSkeleton();
	if (RefSk.FindBoneIndex(FName(*BoneName)) == INDEX_NONE)
	{
		return MakeErrorJson(FString::Printf(TEXT("Bone '%s' not found on skeleton '%s'."), *BoneName, *Skeleton->GetName()));
	}

	// Optional transform — defaults to identity
	auto GetNum = [&Json](const TCHAR* Field, double Default)
	{
		double V = Default;
		Json->TryGetNumberField(Field, V);
		return V;
	};
	FVector Loc(GetNum(TEXT("locX"), 0.0), GetNum(TEXT("locY"), 0.0), GetNum(TEXT("locZ"), 0.0));
	FRotator Rot(GetNum(TEXT("rotPitch"), 0.0), GetNum(TEXT("rotYaw"), 0.0), GetNum(TEXT("rotRoll"), 0.0));
	FVector Scale(GetNum(TEXT("scaleX"), 1.0), GetNum(TEXT("scaleY"), 1.0), GetNum(TEXT("scaleZ"), 1.0));

	bool bOverwrite = true;
	Json->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	const int32 ExistingIdx = FindSocketIndex(Skeleton, FName(*SocketName));
	bool bUpdated = false;
	bool bCreated = false;

	if (ExistingIdx != INDEX_NONE)
	{
		if (!bOverwrite)
		{
			return MakeErrorJson(FString::Printf(TEXT("Socket '%s' already exists on skeleton '%s' (set overwrite=true to update)."), *SocketName, *Skeleton->GetName()));
		}
		bUpdated = true;
	}
	else
	{
		bCreated = true;
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetName());
	Result->SetStringField(TEXT("path"), ResolvedPath);
	Result->SetStringField(TEXT("socketName"), SocketName);
	Result->SetStringField(TEXT("bone"), BoneName);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("updated"), bUpdated);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);

	if (bDryRun)
	{
		Result->SetBoolField(TEXT("success"), true);
		return JsonToString(Result);
	}

	Skeleton->Modify();
	USkeletalMeshSocket* Socket = nullptr;
	if (ExistingIdx != INDEX_NONE)
	{
		Socket = Skeleton->Sockets[ExistingIdx];
	}
	else
	{
		Socket = NewObject<USkeletalMeshSocket>(Skeleton);
		Socket->SetFlags(RF_Transactional);
		Skeleton->Sockets.Add(Socket);
	}
	Socket->Modify();
	Socket->SocketName = FName(*SocketName);
	Socket->BoneName = FName(*BoneName);
	Socket->RelativeLocation = Loc;
	Socket->RelativeRotation = Rot;
	Socket->RelativeScale = Scale;

	Skeleton->MarkPackageDirty();
	const bool bSaved = SaveGenericPackage(Skeleton);

	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("success"), bSaved);
	if (!bSaved)
	{
		Result->SetStringField(TEXT("error"), TEXT("Failed to save USkeleton package. See log."));
	}
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleRemoveSkeletonSocket(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString ResolvedPath, ResolveError;
	USkeleton* Skeleton = ResolveSkeletonFromJson(Json, ResolvedPath, ResolveError);
	if (!Skeleton)
	{
		return MakeErrorJson(ResolveError);
	}

	FString SocketName;
	if (!Json->TryGetStringField(TEXT("socketName"), SocketName) || SocketName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'socketName'."));
	}

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	const int32 Idx = FindSocketIndex(Skeleton, FName(*SocketName));
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetName());
	Result->SetStringField(TEXT("path"), ResolvedPath);
	Result->SetStringField(TEXT("socketName"), SocketName);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);

	if (Idx == INDEX_NONE)
	{
		Result->SetBoolField(TEXT("removed"), false);
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), FString::Printf(TEXT("Socket '%s' not found."), *SocketName));
		return JsonToString(Result);
	}

	if (bDryRun)
	{
		Result->SetBoolField(TEXT("removed"), true);
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("saved"), false);
		return JsonToString(Result);
	}

	Skeleton->Modify();
	Skeleton->Sockets.RemoveAt(Idx);
	Skeleton->MarkPackageDirty();
	const bool bSaved = SaveGenericPackage(Skeleton);

	Result->SetBoolField(TEXT("removed"), true);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("success"), bSaved);
	if (!bSaved)
	{
		Result->SetStringField(TEXT("error"), TEXT("Removed in memory but failed to save USkeleton package. See log."));
	}
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleCopySkeletonSockets(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	// Resolve source — accept fromPath/fromName
	FString FromPath, FromName, FromResolved;
	Json->TryGetStringField(TEXT("fromPath"), FromPath);
	Json->TryGetStringField(TEXT("fromName"), FromName);
	if (FromPath.IsEmpty() && FromName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'fromPath' or 'fromName'."));
	}
	TSharedRef<FJsonObject> FromJson = MakeShared<FJsonObject>();
	if (!FromPath.IsEmpty()) FromJson->SetStringField(TEXT("path"), FromPath);
	if (!FromName.IsEmpty()) FromJson->SetStringField(TEXT("name"), FromName);
	FString FromErr;
	USkeleton* Source = ResolveSkeletonFromJson(FromJson, FromResolved, FromErr);
	if (!Source) return MakeErrorJson(FString::Printf(TEXT("Source: %s"), *FromErr));

	// Resolve destination — accept toPath/toName
	FString ToPath, ToName, ToResolved;
	Json->TryGetStringField(TEXT("toPath"), ToPath);
	Json->TryGetStringField(TEXT("toName"), ToName);
	if (ToPath.IsEmpty() && ToName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing 'toPath' or 'toName'."));
	}
	TSharedRef<FJsonObject> ToJson = MakeShared<FJsonObject>();
	if (!ToPath.IsEmpty()) ToJson->SetStringField(TEXT("path"), ToPath);
	if (!ToName.IsEmpty()) ToJson->SetStringField(TEXT("name"), ToName);
	FString ToErr;
	USkeleton* Dest = ResolveSkeletonFromJson(ToJson, ToResolved, ToErr);
	if (!Dest) return MakeErrorJson(FString::Printf(TEXT("Destination: %s"), *ToErr));

	if (Source == Dest)
	{
		return MakeErrorJson(TEXT("Source and destination are the same skeleton."));
	}

	bool bOverwrite = true;
	Json->TryGetBoolField(TEXT("overwrite"), bOverwrite);
	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Optional filter: copy only listed socket names (case-insensitive). If empty, copy all.
	TSet<FString> Filter;
	const TArray<TSharedPtr<FJsonValue>>* OnlyArr = nullptr;
	if (Json->TryGetArrayField(TEXT("only"), OnlyArr))
	{
		for (const TSharedPtr<FJsonValue>& V : *OnlyArr)
		{
			Filter.Add(V->AsString().ToLower());
		}
	}

	const FReferenceSkeleton& DestRef = Dest->GetReferenceSkeleton();

	TArray<TSharedPtr<FJsonValue>> Created;
	TArray<TSharedPtr<FJsonValue>> Updated;
	TArray<TSharedPtr<FJsonValue>> Skipped;
	TArray<TSharedPtr<FJsonValue>> MissingBones;

	if (!bDryRun)
	{
		Dest->Modify();
	}

	for (USkeletalMeshSocket* Src : Source->Sockets)
	{
		if (!Src) continue;
		const FString SrcName = Src->SocketName.ToString();
		if (Filter.Num() > 0 && !Filter.Contains(SrcName.ToLower()))
		{
			continue;
		}

		if (DestRef.FindBoneIndex(Src->BoneName) == INDEX_NONE)
		{
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("socket"), SrcName);
			E->SetStringField(TEXT("bone"), Src->BoneName.ToString());
			MissingBones.Add(MakeShared<FJsonValueObject>(E));
			continue;
		}

		const int32 ExistingIdx = FindSocketIndex(Dest, Src->SocketName);
		if (ExistingIdx != INDEX_NONE && !bOverwrite)
		{
			Skipped.Add(MakeShared<FJsonValueString>(SrcName));
			continue;
		}

		USkeletalMeshSocket* Dst = nullptr;
		if (ExistingIdx != INDEX_NONE)
		{
			Dst = Dest->Sockets[ExistingIdx];
			Updated.Add(MakeShared<FJsonValueString>(SrcName));
		}
		else
		{
			Created.Add(MakeShared<FJsonValueString>(SrcName));
		}

		if (!bDryRun)
		{
			if (!Dst)
			{
				Dst = NewObject<USkeletalMeshSocket>(Dest);
				Dst->SetFlags(RF_Transactional);
				Dest->Sockets.Add(Dst);
			}
			Dst->Modify();
			Dst->SocketName = Src->SocketName;
			Dst->BoneName = Src->BoneName;
			Dst->RelativeLocation = Src->RelativeLocation;
			Dst->RelativeRotation = Src->RelativeRotation;
			Dst->RelativeScale = Src->RelativeScale;
		}
	}

	bool bSaved = false;
	if (!bDryRun && (Created.Num() > 0 || Updated.Num() > 0))
	{
		Dest->MarkPackageDirty();
		bSaved = SaveGenericPackage(Dest);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("from"), Source->GetName());
	Result->SetStringField(TEXT("to"), Dest->GetName());
	Result->SetStringField(TEXT("fromPath"), FromResolved);
	Result->SetStringField(TEXT("toPath"), ToResolved);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetBoolField(TEXT("overwrite"), bOverwrite);
	Result->SetArrayField(TEXT("created"), Created);
	Result->SetArrayField(TEXT("updated"), Updated);
	Result->SetArrayField(TEXT("skipped"), Skipped);
	Result->SetArrayField(TEXT("missingBones"), MissingBones);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("success"), bDryRun ? true : (bSaved || (Created.Num() == 0 && Updated.Num() == 0)));
	return JsonToString(Result);
}
