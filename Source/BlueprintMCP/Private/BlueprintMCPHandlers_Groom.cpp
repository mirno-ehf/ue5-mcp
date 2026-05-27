// BlueprintMCPHandlers_Groom.cpp — Groom Binding asset tools
// Provides: listGroomBindings, duplicateGroomBinding, setGroomBindingTargetMesh
//
// Uses UObject reflection throughout so the HairStrandsCore plugin is not a
// hard compile-time dependency — tools work as long as the plugin is loaded at
// runtime and the Groom module is present in the project.

#include "BlueprintMCPServer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace
{
	/** True if the asset class name contains "GroomBinding" (case-insensitive). */
	bool IsGroomBindingClass(const FTopLevelAssetPath& ClassPath)
	{
		const FString ClassName = ClassPath.GetAssetName().ToString();
		return ClassName.Contains(TEXT("GroomBinding"), ESearchCase::IgnoreCase);
	}

	/** Collect all GroomBindingAsset entries from the asset registry. */
	TArray<FAssetData> FindAllGroomBindings(IAssetRegistry& AR)
	{
		TArray<FAssetData> Result;

		// Primary search: filter by exact class path (UE5.7 HairStrandsCore)
		FARFilter Filter;
		Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/HairStrandsCore"), TEXT("GroomBindingAsset")));
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(TEXT("/Game"));
		AR.GetAssets(Filter, Result);

		if (Result.Num() > 0)
		{
			return Result;
		}

		// Fallback: scan everything under /Game and match by class name substring.
		// Handles alternate module names across engine versions.
		TArray<FAssetData> AllAssets;
		FARFilter FallbackFilter;
		FallbackFilter.bRecursivePaths = true;
		FallbackFilter.PackagePaths.Add(TEXT("/Game"));
		AR.GetAssets(FallbackFilter, AllAssets);

		for (const FAssetData& AD : AllAssets)
		{
			if (IsGroomBindingClass(AD.AssetClassPath))
			{
				Result.Add(AD);
			}
		}
		return Result;
	}

	/** Read a named UObject* property via reflection. Returns empty string on failure. */
	FString GetObjectPropertyPath(UObject* Asset, FName PropName)
	{
		if (!Asset) return FString();
		FProperty* Prop = Asset->GetClass()->FindPropertyByName(PropName);
		FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
		if (!ObjProp) return FString();
		UObject* Value = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Asset));
		return Value ? Value->GetPathName() : FString();
	}

	/** Serialize one groom binding FAssetData into a JSON object. Loads the asset
	 *  to read GroomAsset / TargetSkeletalMesh / SourceSkeletalMesh fields. */
	TSharedPtr<FJsonObject> SerializeGroomBinding(const FAssetData& AD)
	{
		TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("assetPath"),  AD.GetSoftObjectPath().ToString());
		J->SetStringField(TEXT("packagePath"), AD.PackagePath.ToString());
		J->SetStringField(TEXT("name"),       AD.AssetName.ToString());
		J->SetStringField(TEXT("assetClass"), AD.AssetClassPath.ToString());

		// Load the asset to read inner references (may already be in memory).
		UObject* Asset = AD.GetAsset();
		if (Asset)
		{
			J->SetStringField(TEXT("groomAsset"),          GetObjectPropertyPath(Asset, TEXT("GroomAsset")));
			J->SetStringField(TEXT("targetSkeletalMesh"),  GetObjectPropertyPath(Asset, TEXT("TargetSkeletalMesh")));
			J->SetStringField(TEXT("sourceSkeletalMesh"),  GetObjectPropertyPath(Asset, TEXT("SourceSkeletalMesh")));
		}
		else
		{
			J->SetStringField(TEXT("groomAsset"),         FString());
			J->SetStringField(TEXT("targetSkeletalMesh"), FString());
			J->SetStringField(TEXT("sourceSkeletalMesh"), FString());
		}
		return J;
	}

	/** Find a single groom binding by exact package path or asset name. */
	TOptional<FAssetData> FindGroomBinding(IAssetRegistry& AR, const FString& AssetPath)
	{
		TArray<FAssetData> All = FindAllGroomBindings(AR);
		for (const FAssetData& AD : All)
		{
			const FString SoftPath = AD.GetSoftObjectPath().ToString();
			const FString PkgPath  = AD.PackageName.ToString();
			const FString Name     = AD.AssetName.ToString();

			// Accept full soft-object path, package path, or bare name.
			if (SoftPath == AssetPath || PkgPath == AssetPath || Name == AssetPath)
			{
				return AD;
			}
		}
		return TOptional<FAssetData>();
	}
} // namespace

// ---------------------------------------------------------------------------
// HandleListGroomBindings — GET-style, query params
// ---------------------------------------------------------------------------

FString FBlueprintMCPServer::HandleListGroomBindings(const TMap<FString, FString>& Params)
{
	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> Bindings = FindAllGroomBindings(AR);

	// Optional name filter
	const FString* QueryPtr = Params.Find(TEXT("query"));
	const FString  Query    = QueryPtr ? QueryPtr->ToLower() : FString();

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FAssetData& AD : Bindings)
	{
		if (!Query.IsEmpty())
		{
			const FString LowerName = AD.AssetName.ToString().ToLower();
			if (!LowerName.Contains(Query))
			{
				continue;
			}
		}
		Items.Add(MakeShared<FJsonValueObject>(SerializeGroomBinding(AD)));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"),    Items.Num());
	Result->SetArrayField(TEXT("bindings"), Items);
	return JsonToString(Result);
}

// ---------------------------------------------------------------------------
// HandleDuplicateGroomBinding — POST
// ---------------------------------------------------------------------------
// Body: { "assetPath": "/Game/.../GB_Src", "newName": "GB_Dst",
//          "newFolder": "/Game/.../Hair/" }   ← newFolder is optional

FString FBlueprintMCPServer::HandleDuplicateGroomBinding(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	const FString AssetPath = Json->GetStringField(TEXT("assetPath"));
	const FString NewName   = Json->GetStringField(TEXT("newName"));

	if (AssetPath.IsEmpty() || NewName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: assetPath, newName"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TOptional<FAssetData> Found = FindGroomBinding(AR, AssetPath);
	if (!Found.IsSet())
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Groom binding asset not found: '%s'"), *AssetPath));
	}

	UObject* OriginalAsset = Found.GetValue().GetAsset();
	if (!OriginalAsset)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to load groom binding asset: '%s'"), *AssetPath));
	}

	// Determine destination folder (default: same as source).
	FString NewFolder;
	if (Json->HasField(TEXT("newFolder")))
	{
		NewFolder = Json->GetStringField(TEXT("newFolder"));
		// Strip trailing slash
		NewFolder.RemoveFromEnd(TEXT("/"));
	}
	else
	{
		NewFolder = Found.GetValue().PackagePath.ToString();
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP Groom: Duplicating '%s' -> '%s/%s'"),
		*AssetPath, *NewFolder, *NewName);

	FAssetToolsModule& ATM = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	UObject* DupAsset = ATM.Get().DuplicateAsset(NewName, NewFolder, OriginalAsset);

	if (!DupAsset)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("AssetTools failed to duplicate '%s' to '%s/%s'. Check the destination path is valid and does not already exist."),
			*AssetPath, *NewFolder, *NewName));
	}

	const bool bSaved = SaveGenericPackage(DupAsset);
	const FString NewPath = FString::Printf(TEXT("%s/%s"), *NewFolder, *NewName);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"),      true);
	Result->SetStringField(TEXT("originalPath"), AssetPath);
	Result->SetStringField(TEXT("newPath"),    NewPath);
	Result->SetStringField(TEXT("newName"),    NewName);
	Result->SetBoolField(TEXT("saved"),        bSaved);
	if (!bSaved)
	{
		Result->SetStringField(TEXT("warning"),
			TEXT("Asset was duplicated in memory but could not be saved to disk. Open the editor and save manually."));
	}
	return JsonToString(Result);
}

// ---------------------------------------------------------------------------
// HandleSetGroomBindingTargetMesh — POST
// ---------------------------------------------------------------------------
// Body: { "assetPath": "/Game/.../GB_Regina",
//          "targetMeshPath": "/Game/.../SKM_Regina",
//          "sourceMeshPath": "/Game/.../SKM_Source"  }  ← sourceMeshPath optional

FString FBlueprintMCPServer::HandleSetGroomBindingTargetMesh(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	const FString AssetPath      = Json->GetStringField(TEXT("assetPath"));
	const FString TargetMeshPath = Json->GetStringField(TEXT("targetMeshPath"));

	if (AssetPath.IsEmpty() || TargetMeshPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: assetPath, targetMeshPath"));
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TOptional<FAssetData> Found = FindGroomBinding(AR, AssetPath);
	if (!Found.IsSet())
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Groom binding asset not found: '%s'"), *AssetPath));
	}

	UObject* BindingAsset = Found.GetValue().GetAsset();
	if (!BindingAsset)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to load groom binding asset: '%s'"), *AssetPath));
	}

	// ---- TargetSkeletalMesh ----
	FProperty* TargetProp = BindingAsset->GetClass()->FindPropertyByName(TEXT("TargetSkeletalMesh"));
	FObjectProperty* TargetObjProp = CastField<FObjectProperty>(TargetProp);
	if (!TargetObjProp)
	{
		return MakeErrorJson(TEXT("TargetSkeletalMesh property not found on the asset. "
			"Ensure the HairStrands plugin is loaded and the asset is a valid GroomBindingAsset."));
	}

	// Capture old value before overwriting
	const FString OldTargetPath = GetObjectPropertyPath(BindingAsset, TEXT("TargetSkeletalMesh"));

	UObject* NewTargetMesh = FindObject<UObject>(nullptr, *TargetMeshPath);
	if (!NewTargetMesh)
	{
		NewTargetMesh = LoadObject<UObject>(nullptr, *TargetMeshPath);
	}
	if (!NewTargetMesh)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Could not find or load skeletal mesh: '%s'"), *TargetMeshPath));
	}

	BindingAsset->Modify();
	TargetObjProp->SetObjectPropertyValue(
		TargetObjProp->ContainerPtrToValuePtr<void>(BindingAsset), NewTargetMesh);

	// ---- SourceSkeletalMesh (optional) ----
	FString OldSourcePath;
	FString NewSourcePath;
	if (Json->HasField(TEXT("sourceMeshPath")))
	{
		const FString SourceMeshPath = Json->GetStringField(TEXT("sourceMeshPath"));
		OldSourcePath = GetObjectPropertyPath(BindingAsset, TEXT("SourceSkeletalMesh"));

		if (!SourceMeshPath.IsEmpty())
		{
			FProperty* SourceProp = BindingAsset->GetClass()->FindPropertyByName(TEXT("SourceSkeletalMesh"));
			FObjectProperty* SourceObjProp = CastField<FObjectProperty>(SourceProp);
			if (SourceObjProp)
			{
				UObject* NewSourceMesh = FindObject<UObject>(nullptr, *SourceMeshPath);
				if (!NewSourceMesh)
				{
					NewSourceMesh = LoadObject<UObject>(nullptr, *SourceMeshPath);
				}
				if (NewSourceMesh)
				{
					SourceObjProp->SetObjectPropertyValue(
						SourceObjProp->ContainerPtrToValuePtr<void>(BindingAsset), NewSourceMesh);
					NewSourcePath = SourceMeshPath;
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP Groom: sourceMeshPath '%s' could not be loaded — skipped"), *SourceMeshPath);
				}
			}
		}
	}

	BindingAsset->MarkPackageDirty();
	const bool bSaved = SaveGenericPackage(BindingAsset);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP Groom: SetTargetMesh '%s' -> '%s', save %s"),
		*AssetPath, *TargetMeshPath, bSaved ? TEXT("OK") : TEXT("FAILED"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"),          true);
	Result->SetStringField(TEXT("asset"),          AssetPath);
	Result->SetStringField(TEXT("oldTargetMesh"),  OldTargetPath);
	Result->SetStringField(TEXT("newTargetMesh"),  TargetMeshPath);
	Result->SetBoolField(TEXT("saved"),            bSaved);

	if (!OldSourcePath.IsEmpty() || !NewSourcePath.IsEmpty())
	{
		Result->SetStringField(TEXT("oldSourceMesh"), OldSourcePath);
		Result->SetStringField(TEXT("newSourceMesh"), NewSourcePath);
	}

	Result->SetStringField(TEXT("note"),
		TEXT("The binding geometry data is now stale. Open the asset in the Unreal Editor "
		     "and click 'Rebuild Binding' (or re-bake via the Groom binding editor) "
		     "to regenerate binding data for the new target mesh."));

	return JsonToString(Result);
}

// ---------------------------------------------------------------------------
// HandleRebuildGroomBindings — POST
// ---------------------------------------------------------------------------
// Body forms (any of these):
//   { "assetPath":  "/Game/.../GB_Foo" }
//   { "assetPaths": ["/Game/.../GB_Foo", "/Game/.../GB_Bar"] }
//   { "query":      "TeenFemale" }       // substring filter (case-insensitive) over name
//
// For each matched binding: invalidate, rebuild, wait for compilation,
// then save the package.

#include "GroomBindingAsset.h"
#include "GroomBindingCompiler.h"
#include "GroomAsset.h"

namespace
{
	// Derive the matching Groom asset path from a binding asset path.
	// Convention: /Game/.../<Category>/<GroomName>/Bindings/GB_<GroomName>_<MeshTag>
	//   -> /Game/.../<Category>/<GroomName>/<GroomName>
	// Returns empty string if the path doesn't follow the convention.
	FString DeriveGroomPathFromBindingPath(const FString& BindingPath)
	{
		// Split on '/'
		TArray<FString> Parts;
		BindingPath.ParseIntoArray(Parts, TEXT("/"), true);
		if (Parts.Num() < 4) return FString();
		// Expect last-1 segment to be "Bindings"
		if (!Parts.Last(1).Equals(TEXT("Bindings"), ESearchCase::IgnoreCase))
		{
			return FString();
		}
		// Groom folder name is the segment before "Bindings"
		const FString GroomName = Parts.Last(2);
		// Rebuild path with last two segments dropped and GroomName appended twice
		TArray<FString> NewParts(Parts);
		NewParts.RemoveAt(NewParts.Num() - 1); // drop "GB_*_*"
		NewParts.RemoveAt(NewParts.Num() - 1); // drop "Bindings"
		// Now last segment is GroomName; append GroomName as the asset file
		FString Result = TEXT("/") + FString::Join(NewParts, TEXT("/")) + TEXT("/") + GroomName;
		return Result;
	}
}

FString FBlueprintMCPServer::HandleRebuildGroomBindings(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	// Collect target paths from any of the three input forms.
	TArray<FString> TargetPaths;
	{
		FString Single;
		if (Json->TryGetStringField(TEXT("assetPath"), Single) && !Single.IsEmpty())
		{
			TargetPaths.Add(Single);
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Json->TryGetArrayField(TEXT("assetPaths"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S = V->AsString();
				if (!S.IsEmpty()) TargetPaths.Add(S);
			}
		}
	}

	FString Query;
	Json->TryGetStringField(TEXT("query"), Query);

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Optional field configuration applied before rebuild
	FString TargetMeshPath, SourceMeshPath, ExplicitGroomPath;
	bool bAutoDetectGroom = false;
	Json->TryGetStringField(TEXT("targetMeshPath"), TargetMeshPath);
	Json->TryGetStringField(TEXT("sourceMeshPath"), SourceMeshPath);
	Json->TryGetStringField(TEXT("groomPath"), ExplicitGroomPath);
	Json->TryGetBoolField(TEXT("autoDetectGroom"), bAutoDetectGroom);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	// Resolve to UGroomBindingAsset list.
	TArray<UGroomBindingAsset*> Assets;
	TArray<TSharedPtr<FJsonValue>> NotFound;

	auto AddByPath = [&](const FString& P)
	{
		TOptional<FAssetData> Found = FindGroomBinding(AR, P);
		if (!Found.IsSet())
		{
			NotFound.Add(MakeShared<FJsonValueString>(P));
			return;
		}
		UObject* O = Found.GetValue().GetAsset();
		UGroomBindingAsset* Binding = Cast<UGroomBindingAsset>(O);
		if (!Binding)
		{
			NotFound.Add(MakeShared<FJsonValueString>(P + TEXT(" (not a UGroomBindingAsset)")));
			return;
		}
		Assets.AddUnique(Binding);
	};

	for (const FString& P : TargetPaths)
	{
		AddByPath(P);
	}

	if (!Query.IsEmpty())
	{
		const FString QLow = Query.ToLower();
		TArray<FAssetData> All = FindAllGroomBindings(AR);
		for (const FAssetData& AD : All)
		{
			if (AD.AssetName.ToString().ToLower().Contains(QLow))
			{
				UObject* O = AD.GetAsset();
				if (UGroomBindingAsset* Binding = Cast<UGroomBindingAsset>(O))
				{
					Assets.AddUnique(Binding);
				}
			}
		}
	}

	if (Assets.Num() == 0 && NotFound.Num() == 0)
	{
		return MakeErrorJson(TEXT("Specify 'assetPath', 'assetPaths' (array), or 'query' (substring)."));
	}

	TArray<TSharedPtr<FJsonValue>> Rebuilt;
	TArray<TSharedPtr<FJsonValue>> SavedOK;
	TArray<TSharedPtr<FJsonValue>> SaveFailed;
	TArray<TSharedPtr<FJsonValue>> BuildFailed;
	TArray<TSharedPtr<FJsonValue>> GroomNotFound;

	// Pre-load target/source meshes once if supplied.
	USkeletalMesh* SharedTargetMesh = nullptr;
	if (!TargetMeshPath.IsEmpty())
	{
		SharedTargetMesh = LoadObject<USkeletalMesh>(nullptr, *TargetMeshPath);
		if (!SharedTargetMesh)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not load targetMeshPath '%s'"), *TargetMeshPath));
		}
	}
	USkeletalMesh* SharedSourceMesh = nullptr;
	if (!SourceMeshPath.IsEmpty())
	{
		SharedSourceMesh = LoadObject<USkeletalMesh>(nullptr, *SourceMeshPath);
		if (!SharedSourceMesh)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not load sourceMeshPath '%s'"), *SourceMeshPath));
		}
	}
	UGroomAsset* SharedExplicitGroom = nullptr;
	if (!ExplicitGroomPath.IsEmpty())
	{
		SharedExplicitGroom = LoadObject<UGroomAsset>(nullptr, *ExplicitGroomPath);
		if (!SharedExplicitGroom)
		{
			return MakeErrorJson(FString::Printf(TEXT("Could not load groomPath '%s'"), *ExplicitGroomPath));
		}
	}

	if (!bDryRun)
	{
		// Phase 1: apply field configuration on each asset using the proper API.
		for (UGroomBindingAsset* Asset : Assets)
		{
			if (!Asset) continue;
			Asset->Modify();

			// Resolve groom for this binding (per-asset auto-detect, or shared explicit)
			UGroomAsset* GroomForThis = SharedExplicitGroom;
			if (!GroomForThis && bAutoDetectGroom && !Asset->GetGroom())
			{
				const FString AssetPath = Asset->GetPathName();
				const FString DerivedGroomPath = DeriveGroomPathFromBindingPath(AssetPath);
				if (!DerivedGroomPath.IsEmpty())
				{
					GroomForThis = LoadObject<UGroomAsset>(nullptr, *DerivedGroomPath);
					if (!GroomForThis)
					{
						GroomNotFound.Add(MakeShared<FJsonValueString>(Asset->GetName() + TEXT(" -> ") + DerivedGroomPath));
					}
				}
			}
			if (GroomForThis)
			{
				Asset->SetGroom(GroomForThis);
			}
			if (SharedTargetMesh)
			{
				Asset->SetTargetSkeletalMesh(SharedTargetMesh);
			}
			if (SharedSourceMesh)
			{
				Asset->SetSourceSkeletalMesh(SharedSourceMesh);
			}
		}

		// Phase 2: kick off builds. Capture completion via native delegate.
		struct FBuildState
		{
			TWeakObjectPtr<UGroomBindingAsset> Asset;
			bool bDone = false;
			bool bSuccess = false;
		};
		TArray<TSharedPtr<FBuildState>> States;
		States.Reserve(Assets.Num());

		for (UGroomBindingAsset* Asset : Assets)
		{
			if (!Asset) continue;
			Asset->InvalidateBinding();

			TSharedPtr<FBuildState> State = MakeShared<FBuildState>();
			State->Asset = Asset;
			States.Add(State);

			FOnGroomBindingAssetBuildCompleteNative NativeDelegate;
			TSharedPtr<FBuildState> StateCapture = State;
			NativeDelegate.BindLambda([StateCapture](UGroomBindingAsset*, EGroomBindingAssetBuildResult Result)
			{
				StateCapture->bDone = true;
				StateCapture->bSuccess = (Result == EGroomBindingAssetBuildResult::Succeeded);
			});
			Asset->Build(NativeDelegate);
		}

		// Phase 3: block until all async builds are done.
		TArray<UGroomBindingAsset*> AssetsView = Assets;
		FGroomBindingCompilingManager::Get().FinishCompilation(AssetsView);

		// Phase 4: save each that built successfully.
		for (const TSharedPtr<FBuildState>& State : States)
		{
			UGroomBindingAsset* Asset = State->Asset.Get();
			if (!Asset) continue;

			const FString Name = Asset->GetName();
			bool bConsiderRebuilt = State->bDone ? State->bSuccess : true;

			if (!bConsiderRebuilt)
			{
				BuildFailed.Add(MakeShared<FJsonValueString>(Name));
				continue;
			}

			Rebuilt.Add(MakeShared<FJsonValueString>(Name));
			Asset->MarkPackageDirty();
			const bool bSaved = SaveGenericPackage(Asset);
			(bSaved ? SavedOK : SaveFailed).Add(MakeShared<FJsonValueString>(Name));
		}
	}
	else
	{
		// Dry run — just list what would be touched.
		for (UGroomBindingAsset* Asset : Assets)
		{
			if (!Asset) continue;
			Rebuilt.Add(MakeShared<FJsonValueString>(Asset->GetName()));
			if (bAutoDetectGroom && !Asset->GetGroom())
			{
				const FString Derived = DeriveGroomPathFromBindingPath(Asset->GetPathName());
				if (!Derived.IsEmpty() && !LoadObject<UGroomAsset>(nullptr, *Derived))
				{
					GroomNotFound.Add(MakeShared<FJsonValueString>(Asset->GetName() + TEXT(" -> ") + Derived));
				}
			}
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetNumberField(TEXT("matched"), Assets.Num());
	Result->SetArrayField(TEXT("rebuilt"), Rebuilt);
	Result->SetArrayField(TEXT("saved"), SavedOK);
	Result->SetArrayField(TEXT("saveFailed"), SaveFailed);
	Result->SetArrayField(TEXT("buildFailed"), BuildFailed);
	Result->SetArrayField(TEXT("notFound"), NotFound);
	Result->SetArrayField(TEXT("groomNotFound"), GroomNotFound);
	Result->SetBoolField(TEXT("success"), BuildFailed.Num() == 0 && SaveFailed.Num() == 0 && GroomNotFound.Num() == 0);
	return JsonToString(Result);
}
