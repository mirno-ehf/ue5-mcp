#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "LevelEditor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleStartPIE — start Play In Editor
// ============================================================

FString FBlueprintMCPServer::HandleStartPIE(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: start_pie()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("start_pie requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	if (GEditor->PlayWorld)
	{
		return MakeErrorJson(TEXT("PIE is already running. Use stop_pie first or is_pie_running to check status."));
	}

	// Start PIE using the default settings
	FRequestPlaySessionParams Params;
	Params.WorldType = EPlaySessionWorldType::PlayInEditor;
	Params.DestinationSlateViewport = nullptr; // Use default

	GUnrealEd->RequestPlaySession(Params);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("status"), TEXT("PIE session requested. It may take a moment to start."));

	return JsonToString(Result);
}

// ============================================================
// HandleStopPIE — stop Play In Editor
// ============================================================

FString FBlueprintMCPServer::HandleStopPIE(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: stop_pie()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("stop_pie requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	if (!GEditor->PlayWorld)
	{
		return MakeErrorJson(TEXT("PIE is not running."));
	}

	GUnrealEd->RequestEndPlayMap();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("status"), TEXT("PIE stop requested."));

	return JsonToString(Result);
}

// ============================================================
// HandleIsPIERunning — check if PIE is active
// ============================================================

FString FBlueprintMCPServer::HandleIsPIERunning(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: is_pie_running()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("is_pie_running requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	bool bIsRunning = GEditor->PlayWorld != nullptr;
	bool bIsPaused = false;
	if (bIsRunning && GEditor->PlayWorld)
	{
		bIsPaused = GEditor->PlayWorld->IsPaused();
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("running"), bIsRunning);
	Result->SetBoolField(TEXT("paused"), bIsPaused);

	return JsonToString(Result);
}

// ============================================================
// HandlePIEPause — pause/unpause PIE
// ============================================================

FString FBlueprintMCPServer::HandlePIEPause(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	bool bPause = true;
	if (!Json->TryGetBoolField(TEXT("paused"), bPause))
	{
		return MakeErrorJson(TEXT("Missing required field: 'paused' (boolean)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: pie_pause(%s)"), bPause ? TEXT("true") : TEXT("false"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("pie_pause requires editor mode."));
	}

	if (!GEditor || !GEditor->PlayWorld)
	{
		return MakeErrorJson(TEXT("PIE is not running. Use start_pie first."));
	}

	GEditor->PlayWorld->bDebugPauseExecution = bPause;

	// Use the proper pause mechanism
	if (bPause)
	{
		GUnrealEd->SetPIEWorldsPaused(true);
	}
	else
	{
		GUnrealEd->SetPIEWorldsPaused(false);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("paused"), bPause);

	return JsonToString(Result);
}
