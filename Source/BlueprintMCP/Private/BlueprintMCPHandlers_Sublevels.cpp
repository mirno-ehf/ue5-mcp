#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "EditorLevelUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleGetLevelInfo — get information about the current level
// ============================================================

FString FBlueprintMCPServer::HandleGetLevelInfo(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_level_info()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("get_level_info requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available."));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("worldName"), World->GetName());
	Result->SetStringField(TEXT("mapName"), World->GetMapName());

	// Persistent level info
	ULevel* PersistentLevel = World->PersistentLevel;
	if (PersistentLevel)
	{
		Result->SetNumberField(TEXT("persistentLevelActorCount"), PersistentLevel->Actors.Num());
	}

	// Streaming levels
	const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
	Result->SetNumberField(TEXT("streamingLevelCount"), StreamingLevels.Num());

	TArray<TSharedPtr<FJsonValue>> LevelArray;
	for (ULevelStreaming* StreamingLevel : StreamingLevels)
	{
		if (!StreamingLevel) continue;

		TSharedRef<FJsonObject> LevelObj = MakeShared<FJsonObject>();
		LevelObj->SetStringField(TEXT("packageName"), StreamingLevel->GetWorldAssetPackageName());
		LevelObj->SetStringField(TEXT("shortName"), FPackageName::GetShortName(StreamingLevel->GetWorldAssetPackageName()));

		bool bIsLoaded = StreamingLevel->GetLoadedLevel() != nullptr;
		bool bIsVisible = StreamingLevel->GetShouldBeVisibleFlag();

		LevelObj->SetBoolField(TEXT("isLoaded"), bIsLoaded);
		LevelObj->SetBoolField(TEXT("isVisible"), bIsVisible);

		if (bIsLoaded && StreamingLevel->GetLoadedLevel())
		{
			LevelObj->SetNumberField(TEXT("actorCount"), StreamingLevel->GetLoadedLevel()->Actors.Num());
		}

		LevelArray.Add(MakeShared<FJsonValueObject>(LevelObj));
	}
	Result->SetArrayField(TEXT("streamingLevels"), LevelArray);

	return JsonToString(Result);
}

// ============================================================
// HandleListSublevels — list all streaming sublevels
// ============================================================

FString FBlueprintMCPServer::HandleListSublevels(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: list_sublevels()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("list_sublevels requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available."));
	}

	const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), StreamingLevels.Num());

	TArray<TSharedPtr<FJsonValue>> LevelArray;
	for (ULevelStreaming* StreamingLevel : StreamingLevels)
	{
		if (!StreamingLevel) continue;

		TSharedRef<FJsonObject> LevelObj = MakeShared<FJsonObject>();
		LevelObj->SetStringField(TEXT("packageName"), StreamingLevel->GetWorldAssetPackageName());
		LevelObj->SetStringField(TEXT("shortName"), FPackageName::GetShortName(StreamingLevel->GetWorldAssetPackageName()));

		bool bIsLoaded = StreamingLevel->GetLoadedLevel() != nullptr;
		bool bIsVisible = StreamingLevel->GetShouldBeVisibleFlag();

		LevelObj->SetBoolField(TEXT("isLoaded"), bIsLoaded);
		LevelObj->SetBoolField(TEXT("isVisible"), bIsVisible);

		// Get the streaming class name (e.g., LevelStreamingDynamic, LevelStreamingAlwaysLoaded)
		LevelObj->SetStringField(TEXT("streamingClass"), StreamingLevel->GetClass()->GetName());

		if (bIsLoaded && StreamingLevel->GetLoadedLevel())
		{
			LevelObj->SetNumberField(TEXT("actorCount"), StreamingLevel->GetLoadedLevel()->Actors.Num());
		}

		LevelArray.Add(MakeShared<FJsonValueObject>(LevelObj));
	}
	Result->SetArrayField(TEXT("sublevels"), LevelArray);

	return JsonToString(Result);
}

// ============================================================
// HandleLoadSublevel — load a streaming sublevel
// ============================================================

FString FBlueprintMCPServer::HandleLoadSublevel(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString LevelName;
	if (!Json->TryGetStringField(TEXT("levelName"), LevelName) || LevelName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'levelName' (package name or short name of the sublevel)."));
	}

	bool bMakeVisible = true;
	Json->TryGetBoolField(TEXT("makeVisible"), bMakeVisible);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: load_sublevel('%s', visible=%s)"),
		*LevelName, bMakeVisible ? TEXT("true") : TEXT("false"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("load_sublevel requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available."));
	}

	// Find the streaming level by name
	ULevelStreaming* FoundLevel = nullptr;
	const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
	for (ULevelStreaming* StreamingLevel : StreamingLevels)
	{
		if (!StreamingLevel) continue;

		FString PackageName = StreamingLevel->GetWorldAssetPackageName();
		FString ShortName = FPackageName::GetShortName(PackageName);

		if (PackageName.Equals(LevelName, ESearchCase::IgnoreCase) ||
			ShortName.Equals(LevelName, ESearchCase::IgnoreCase))
		{
			FoundLevel = StreamingLevel;
			break;
		}
	}

	if (!FoundLevel)
	{
		return MakeErrorJson(FString::Printf(TEXT("Sublevel '%s' not found. Use list_sublevels to see available sublevels."), *LevelName));
	}

	bool bWasLoaded = FoundLevel->GetLoadedLevel() != nullptr;

	// Set streaming flags to load the level
	FoundLevel->SetShouldBeLoaded(true);
	if (bMakeVisible)
	{
		FoundLevel->SetShouldBeVisible(true);
	}

	// Force an update
	World->FlushLevelStreaming(EFlushLevelStreamingType::Full);

	bool bIsNowLoaded = FoundLevel->GetLoadedLevel() != nullptr;

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("levelName"), FoundLevel->GetWorldAssetPackageName());
	Result->SetBoolField(TEXT("wasLoaded"), bWasLoaded);
	Result->SetBoolField(TEXT("isLoaded"), bIsNowLoaded);
	Result->SetBoolField(TEXT("isVisible"), FoundLevel->GetShouldBeVisibleFlag());

	return JsonToString(Result);
}

// ============================================================
// HandleUnloadSublevel — unload a streaming sublevel
// ============================================================

FString FBlueprintMCPServer::HandleUnloadSublevel(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString LevelName;
	if (!Json->TryGetStringField(TEXT("levelName"), LevelName) || LevelName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'levelName' (package name or short name of the sublevel)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: unload_sublevel('%s')"), *LevelName);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("unload_sublevel requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorJson(TEXT("No editor world available."));
	}

	// Find the streaming level by name
	ULevelStreaming* FoundLevel = nullptr;
	const TArray<ULevelStreaming*>& StreamingLevels = World->GetStreamingLevels();
	for (ULevelStreaming* StreamingLevel : StreamingLevels)
	{
		if (!StreamingLevel) continue;

		FString PackageName = StreamingLevel->GetWorldAssetPackageName();
		FString ShortName = FPackageName::GetShortName(PackageName);

		if (PackageName.Equals(LevelName, ESearchCase::IgnoreCase) ||
			ShortName.Equals(LevelName, ESearchCase::IgnoreCase))
		{
			FoundLevel = StreamingLevel;
			break;
		}
	}

	if (!FoundLevel)
	{
		return MakeErrorJson(FString::Printf(TEXT("Sublevel '%s' not found. Use list_sublevels to see available sublevels."), *LevelName));
	}

	bool bWasLoaded = FoundLevel->GetLoadedLevel() != nullptr;

	// Set streaming flags to unload the level
	FoundLevel->SetShouldBeVisible(false);
	FoundLevel->SetShouldBeLoaded(false);

	// Force an update
	World->FlushLevelStreaming(EFlushLevelStreamingType::Full);

	bool bIsNowLoaded = FoundLevel->GetLoadedLevel() != nullptr;

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("levelName"), FoundLevel->GetWorldAssetPackageName());
	Result->SetBoolField(TEXT("wasLoaded"), bWasLoaded);
	Result->SetBoolField(TEXT("isLoaded"), bIsNowLoaded);

	return JsonToString(Result);
}
