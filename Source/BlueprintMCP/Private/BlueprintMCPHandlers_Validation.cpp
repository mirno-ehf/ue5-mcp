#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// SEH wrapper defined in BlueprintMCPServer.cpp — non-static for cross-TU access
#if PLATFORM_WINDOWS
extern int32 TryCompileBlueprintSEH(UBlueprint* BP, EBlueprintCompileOptions Opts);
#endif

// ============================================================
// Log capture device for intercepting UE_LOG output during compilation
// ============================================================

class FCompileLogCapture : public FOutputDevice
{
public:
	TArray<FString> CapturedErrors;
	TArray<FString> CapturedWarnings;

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FString Msg(V);

		if (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Fatal)
		{
			CapturedErrors.Add(Msg);
			return;
		}

		if (Verbosity == ELogVerbosity::Warning)
		{
			if (!Msg.Contains(TEXT("BlueprintMCP:")))
			{
				CapturedWarnings.Add(Msg);
			}
			return;
		}

		static const TCHAR* ErrorPatterns[] = {
			TEXT("Can't connect pins"),
			TEXT("Fixed up function"),
			TEXT("is not compatible with"),
			TEXT("could not find a pin"),
			TEXT("has an invalid"),
			TEXT("orphaned pin"),
			TEXT("is deprecated"),
			TEXT("does not implement"),
			TEXT("Missing function"),
			TEXT("Unable to find"),
			TEXT("Failed to resolve"),
		};

		for (const TCHAR* Pattern : ErrorPatterns)
		{
			if (Msg.Contains(Pattern))
			{
				CapturedWarnings.Add(Msg);
				return;
			}
		}
	}
};

// Helper: validate a single Blueprint and return structured JSON result
static TSharedRef<FJsonObject> ValidateSingleBlueprint(UBlueprint* BP, const FString& BlueprintName)
{
	FCompileLogCapture LogCapture;
	GLog->AddOutputDevice(&LogCapture);

	EBlueprintCompileOptions CompileOpts =
		EBlueprintCompileOptions::SkipSave |
		EBlueprintCompileOptions::SkipGarbageCollection |
		EBlueprintCompileOptions::SkipFiBSearchMetaUpdate;

	bool bCompileCrashed = false;

#if PLATFORM_WINDOWS
	int32 CompileResult = TryCompileBlueprintSEH(BP, CompileOpts);
	if (CompileResult != 0)
	{
		bCompileCrashed = true;
	}
#else
	FKismetEditorUtilities::CompileBlueprint(BP, CompileOpts, nullptr);
#endif

	GLog->RemoveOutputDevice(&LogCapture);

	TArray<TSharedPtr<FJsonValue>> ErrorsArr;
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	TArray<UEdGraph*> AllGraphs;
	BP->GetAllGraphs(AllGraphs);

	for (UEdGraph* Graph : AllGraphs)
	{
		if (!Graph) continue;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Node->bHasCompilerMessage)
			{
				TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
				Msg->SetStringField(TEXT("graph"), Graph->GetName());
				Msg->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
				Msg->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				Msg->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
				Msg->SetStringField(TEXT("message"), Node->ErrorMsg);

				if (Node->ErrorType == EMessageSeverity::Error)
				{
					Msg->SetStringField(TEXT("severity"), TEXT("error"));
					ErrorsArr.Add(MakeShared<FJsonValueObject>(Msg));
				}
				else
				{
					Msg->SetStringField(TEXT("severity"), TEXT("warning"));
					WarningsArr.Add(MakeShared<FJsonValueObject>(Msg));
				}
			}
		}
	}

	for (const FString& LogErr : LogCapture.CapturedErrors)
	{
		TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("source"), TEXT("log"));
		Msg->SetStringField(TEXT("message"), LogErr);
		Msg->SetStringField(TEXT("severity"), TEXT("error"));
		ErrorsArr.Add(MakeShared<FJsonValueObject>(Msg));
	}

	for (const FString& LogWarn : LogCapture.CapturedWarnings)
	{
		TSharedRef<FJsonObject> Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("source"), TEXT("log"));
		Msg->SetStringField(TEXT("message"), LogWarn);
		Msg->SetStringField(TEXT("severity"), TEXT("warning"));
		WarningsArr.Add(MakeShared<FJsonValueObject>(Msg));
	}

	FString StatusStr;
	switch (BP->Status)
	{
		case BS_UpToDate: StatusStr = TEXT("UpToDate"); break;
		case BS_Dirty: StatusStr = TEXT("Dirty"); break;
		case BS_Error: StatusStr = TEXT("Error"); break;
		case BS_Unknown: StatusStr = TEXT("Unknown"); break;
		default: StatusStr = FString::Printf(TEXT("Status_%d"), (int32)BP->Status); break;
	}

	bool bIsValid = (BP->Status == BS_UpToDate) && ErrorsArr.Num() == 0;

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("status"), StatusStr);
	Result->SetBoolField(TEXT("isValid"), bIsValid);
	Result->SetNumberField(TEXT("errorCount"), ErrorsArr.Num());
	Result->SetArrayField(TEXT("errors"), ErrorsArr);
	Result->SetNumberField(TEXT("warningCount"), WarningsArr.Num());
	Result->SetArrayField(TEXT("warnings"), WarningsArr);
	if (bCompileCrashed)
	{
		Result->SetStringField(TEXT("compileWarning"), TEXT("Compilation crashed (SEH caught), results may be incomplete"));
	}
	return Result;
}

// HandleValidateBlueprint — compile without saving, report errors + captured log messages
// ============================================================

FString FBlueprintMCPServer::HandleValidateBlueprint(const FString& Body)
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

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Validating blueprint '%s'"), *BlueprintName);

	TSharedRef<FJsonObject> Result = ValidateSingleBlueprint(BP, BlueprintName);
	return JsonToString(Result);
}

// ============================================================
// HandleValidateAllBlueprints — bulk validation
// ============================================================

FString FBlueprintMCPServer::HandleValidateAllBlueprints(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	// Body is optional — empty body means validate all

	FString Filter;
	bool bCountOnly = false;
	int32 Offset = 0;
	int32 Limit = 0;

	if (Json.IsValid())
	{
		Filter = Json->GetStringField(TEXT("filter"));
		bCountOnly = Json->GetBoolField(TEXT("countOnly"));
		Offset = (int32)Json->GetNumberField(TEXT("offset"));
		Limit = (int32)Json->GetNumberField(TEXT("limit"));
	}

	// First pass: collect matching asset indices (string comparisons only, no GetAsset())
	TArray<int32> MatchingIndices;
	for (int32 i = 0; i < AllBlueprintAssets.Num(); i++)
	{
		const FAssetData& Asset = AllBlueprintAssets[i];
		if (!Filter.IsEmpty())
		{
			FString AssetName = Asset.AssetName.ToString();
			FString PackagePath = Asset.PackageName.ToString();
			if (!PackagePath.Contains(Filter) && !AssetName.Contains(Filter))
			{
				continue;
			}
		}
		MatchingIndices.Add(i);
	}

	int32 TotalMatching = MatchingIndices.Num();

	// countOnly: return count without compiling anything
	if (bCountOnly)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("totalMatching"), TotalMatching);
		if (!Filter.IsEmpty())
		{
			Result->SetStringField(TEXT("filter"), Filter);
		}
		return JsonToString(Result);
	}

	// Compute range
	int32 StartIdx = FMath::Clamp(Offset, 0, TotalMatching);
	int32 EndIdx = (Limit > 0) ? FMath::Min(StartIdx + Limit, TotalMatching) : TotalMatching;

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Bulk validating blueprints (filter: '%s', range: %d-%d of %d matching)"),
		Filter.IsEmpty() ? TEXT("*") : *Filter, StartIdx, EndIdx, TotalMatching);

	TArray<TSharedPtr<FJsonValue>> FailedArr;
	int32 TotalChecked = 0;
	int32 TotalPassed = 0;
	int32 TotalFailed = 0;
	int32 TotalCrashed = 0;

	for (int32 Idx = StartIdx; Idx < EndIdx; Idx++)
	{
		const FAssetData& Asset = AllBlueprintAssets[MatchingIndices[Idx]];
		FString AssetName = Asset.AssetName.ToString();
		FString PackagePath = Asset.PackageName.ToString();

		// Load the Blueprint
		UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
		if (!BP)
		{
			continue;
		}

		TotalChecked++;

		TSharedRef<FJsonObject> Result = ValidateSingleBlueprint(BP, AssetName);

		bool bValid = Result->GetBoolField(TEXT("isValid"));
		int32 Errors = (int32)Result->GetNumberField(TEXT("errorCount"));
		int32 Warnings = (int32)Result->GetNumberField(TEXT("warningCount"));

		if (Result->HasField(TEXT("compileWarning")))
		{
			TotalCrashed++;
		}

		if (bValid && Errors == 0)
		{
			TotalPassed++;
		}
		else
		{
			TotalFailed++;
			// Include path for context in bulk results
			Result->SetStringField(TEXT("path"), PackagePath);
			FailedArr.Add(MakeShared<FJsonValueObject>(Result));
		}

		// Log progress every 50 blueprints
		if (TotalChecked % 50 == 0)
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Validated %d blueprints so far (%d failed)..."),
				TotalChecked, TotalFailed);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Bulk validation complete — %d checked, %d passed, %d failed, %d crashed"),
		TotalChecked, TotalPassed, TotalFailed, TotalCrashed);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("totalMatching"), TotalMatching);
	Result->SetNumberField(TEXT("totalChecked"), TotalChecked);
	Result->SetNumberField(TEXT("totalPassed"), TotalPassed);
	Result->SetNumberField(TEXT("totalFailed"), TotalFailed);
	if (TotalCrashed > 0)
	{
		Result->SetNumberField(TEXT("totalCrashed"), TotalCrashed);
	}
	Result->SetArrayField(TEXT("failed"), FailedArr);
	if (!Filter.IsEmpty())
	{
		Result->SetStringField(TEXT("filter"), Filter);
	}
	return JsonToString(Result);
}
