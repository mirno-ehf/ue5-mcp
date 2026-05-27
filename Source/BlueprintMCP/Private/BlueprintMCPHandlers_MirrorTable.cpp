// MirrorDataTable row editing — list / set / remove. Useful for fixing
// centerline (self-mirror) entries that aren't covered by a table's
// MirrorFindReplaceExpressions, e.g. cc_base_pelvis on a CC4 skeleton.

#include "BlueprintMCPServer.h"

#include "Animation/MirrorDataTable.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	static UMirrorDataTable* LoadMirrorTableByName(const FString& NameOrPath)
	{
		FAssetRegistryModule& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

		const FSoftObjectPath ObjPath(NameOrPath);
		FAssetData ByPath = AssetRegistry.Get().GetAssetByObjectPath(ObjPath);
		if (ByPath.IsValid())
		{
			return Cast<UMirrorDataTable>(ByPath.GetAsset());
		}

		TArray<FAssetData> Found;
		AssetRegistry.Get().GetAssetsByClass(UMirrorDataTable::StaticClass()->GetClassPathName(), Found, true);
		const FString JustName = FPackageName::GetShortName(NameOrPath);
		for (const FAssetData& A : Found)
		{
			if (A.AssetName.ToString().Equals(JustName, ESearchCase::IgnoreCase) ||
				A.GetSoftObjectPath().ToString().Equals(NameOrPath, ESearchCase::IgnoreCase))
			{
				return Cast<UMirrorDataTable>(A.GetAsset());
			}
		}
		return nullptr;
	}

	static EMirrorRowType::Type ParseMirrorRowType(const FString& Text)
	{
		if (Text.Equals(TEXT("AnimationNotify"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("Notify"), ESearchCase::IgnoreCase)) return EMirrorRowType::AnimationNotify;
		if (Text.Equals(TEXT("Curve"), ESearchCase::IgnoreCase)) return EMirrorRowType::Curve;
		if (Text.Equals(TEXT("SyncMarker"), ESearchCase::IgnoreCase)) return EMirrorRowType::SyncMarker;
		if (Text.Equals(TEXT("Custom"), ESearchCase::IgnoreCase)) return EMirrorRowType::Custom;
		return EMirrorRowType::Bone;
	}

	static const TCHAR* MirrorRowTypeToString(EMirrorRowType::Type T)
	{
		switch (T)
		{
		case EMirrorRowType::AnimationNotify: return TEXT("AnimationNotify");
		case EMirrorRowType::Curve:           return TEXT("Curve");
		case EMirrorRowType::SyncMarker:      return TEXT("SyncMarker");
		case EMirrorRowType::Custom:          return TEXT("Custom");
		case EMirrorRowType::Bone:
		default:                              return TEXT("Bone");
		}
	}

	static bool SaveMirrorTablePackage(UMirrorDataTable* Table)
	{
		if (!Table) { return false; }
		UPackage* Pkg = Table->GetOutermost();
		if (!Pkg) { return false; }
		Table->MarkPackageDirty();
		const FString PackageFile = FPackageName::LongPackageNameToFilename(Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		return UPackage::SavePackage(Pkg, Table, *PackageFile, Args);
	}
}

FString FBlueprintMCPServer::HandleListMirrorTableRows(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) { return MakeErrorJson(TEXT("Invalid JSON body")); }
	FString TableName;
	if (!Json->TryGetStringField(TEXT("table"), TableName)) { return MakeErrorJson(TEXT("Missing 'table' field")); }

	UMirrorDataTable* Table = LoadMirrorTableByName(TableName);
	if (!Table) { return MakeErrorJson(FString::Printf(TEXT("Mirror data table not found: %s"), *TableName)); }

	TArray<TSharedPtr<FJsonValue>> RowsArr;
	const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
	for (const TPair<FName, uint8*>& Pair : RowMap)
	{
		if (const FMirrorTableRow* Row = reinterpret_cast<const FMirrorTableRow*>(Pair.Value))
		{
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("rowName"), Pair.Key.ToString());
			O->SetStringField(TEXT("name"), Row->Name.ToString());
			O->SetStringField(TEXT("mirroredName"), Row->MirroredName.ToString());
			O->SetStringField(TEXT("entryType"), MirrorRowTypeToString(Row->MirrorEntryType));
			RowsArr.Add(MakeShared<FJsonValueObject>(O));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("table"), Table->GetPathName());
	Result->SetNumberField(TEXT("rowCount"), RowsArr.Num());
	Result->SetStringField(TEXT("mirrorAxis"),
		Table->MirrorAxis == EAxis::X ? TEXT("X") : Table->MirrorAxis == EAxis::Y ? TEXT("Y") : Table->MirrorAxis == EAxis::Z ? TEXT("Z") : TEXT("None"));
	Result->SetArrayField(TEXT("rows"), RowsArr);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleSetMirrorTableRows(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) { return MakeErrorJson(TEXT("Invalid JSON body")); }

	FString TableName;
	if (!Json->TryGetStringField(TEXT("table"), TableName)) { return MakeErrorJson(TEXT("Missing 'table' field")); }

	const TArray<TSharedPtr<FJsonValue>>* RowsField = nullptr;
	if (!Json->TryGetArrayField(TEXT("rows"), RowsField) || !RowsField) { return MakeErrorJson(TEXT("Missing 'rows' array")); }

	UMirrorDataTable* Table = LoadMirrorTableByName(TableName);
	if (!Table) { return MakeErrorJson(FString::Printf(TEXT("Mirror data table not found: %s"), *TableName)); }

	int32 NumAdded = 0;
	int32 NumUpdated = 0;
	TArray<FString> Warnings;

	Table->Modify();
	for (const TSharedPtr<FJsonValue>& V : *RowsField)
	{
		const TSharedPtr<FJsonObject>* RowObj = nullptr;
		if (!V.IsValid() || !V->TryGetObject(RowObj) || !RowObj || !RowObj->IsValid()) { continue; }

		FString BoneName, MirroredName, EntryTypeStr;
		(*RowObj)->TryGetStringField(TEXT("name"), BoneName);
		(*RowObj)->TryGetStringField(TEXT("mirroredName"), MirroredName);
		(*RowObj)->TryGetStringField(TEXT("entryType"), EntryTypeStr);
		if (BoneName.IsEmpty() || MirroredName.IsEmpty())
		{
			Warnings.Add(FString::Printf(TEXT("Skipped row: name='%s' mirroredName='%s' (both required)"), *BoneName, *MirroredName));
			continue;
		}

		FMirrorTableRow Row;
		Row.Name = FName(*BoneName);
		Row.MirroredName = FName(*MirroredName);
		Row.MirrorEntryType = ParseMirrorRowType(EntryTypeStr);

		const FName RowKey(*BoneName);
		const bool bExisted = Table->GetRowMap().Contains(RowKey);
		Table->RemoveRow(RowKey);
		Table->AddRow(RowKey, Row);
		if (bExisted) { ++NumUpdated; } else { ++NumAdded; }
	}

	// Rebuild BoneToMirrorBoneIndex / curve / notify / sync maps from the updated rows.
	FPropertyChangedEvent ChangeEvent(nullptr);
	Table->PostEditChangeProperty(ChangeEvent);

	const bool bSaved = SaveMirrorTablePackage(Table);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("table"), Table->GetPathName());
	Result->SetNumberField(TEXT("added"), NumAdded);
	Result->SetNumberField(TEXT("updated"), NumUpdated);
	Result->SetNumberField(TEXT("totalRows"), Table->GetRowMap().Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ErrArr;
		for (const FString& E : Warnings) { ErrArr.Add(MakeShared<FJsonValueString>(E)); }
		Result->SetArrayField(TEXT("warnings"), ErrArr);
	}
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleRemoveMirrorTableRows(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) { return MakeErrorJson(TEXT("Invalid JSON body")); }
	FString TableName;
	if (!Json->TryGetStringField(TEXT("table"), TableName)) { return MakeErrorJson(TEXT("Missing 'table' field")); }
	const TArray<TSharedPtr<FJsonValue>>* NamesField = nullptr;
	if (!Json->TryGetArrayField(TEXT("rowNames"), NamesField) || !NamesField) { return MakeErrorJson(TEXT("Missing 'rowNames' array")); }

	UMirrorDataTable* Table = LoadMirrorTableByName(TableName);
	if (!Table) { return MakeErrorJson(FString::Printf(TEXT("Mirror data table not found: %s"), *TableName)); }

	int32 NumRemoved = 0;
	Table->Modify();
	for (const TSharedPtr<FJsonValue>& V : *NamesField)
	{
		FString N;
		if (!V.IsValid() || !V->TryGetString(N) || N.IsEmpty()) { continue; }
		const FName RowKey(*N);
		if (Table->GetRowMap().Contains(RowKey))
		{
			Table->RemoveRow(RowKey);
			++NumRemoved;
		}
	}

	FPropertyChangedEvent ChangeEvent(nullptr);
	Table->PostEditChangeProperty(ChangeEvent);
	const bool bSaved = SaveMirrorTablePackage(Table);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("table"), Table->GetPathName());
	Result->SetNumberField(TEXT("removed"), NumRemoved);
	Result->SetNumberField(TEXT("totalRows"), Table->GetRowMap().Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
