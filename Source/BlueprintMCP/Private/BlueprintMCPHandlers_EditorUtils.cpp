#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

// ============================================================
// Helper — find an actor by label
// ============================================================

static AActor* FindActorByLabelEditorUtils(const FString& Label, FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("Editor not available.");
		return nullptr;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		OutError = TEXT("No editor world available.");
		return nullptr;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == Label)
		{
			return Actor;
		}
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Actor;
		}
	}

	OutError = FString::Printf(TEXT("Actor with label '%s' not found."), *Label);
	return nullptr;
}

// ============================================================
// HandleFocusActor — focus the viewport on an actor
// ============================================================

FString FBlueprintMCPServer::HandleFocusActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: focus_actor('%s')"), *ActorLabel);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("focus_actor requires editor mode."));
	}

	FString Error;
	AActor* Actor = FindActorByLabelEditorUtils(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);

	// Select the actor and focus
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, true);
	GEditor->MoveViewportCamerasToActor(*Actor, false);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);

	FVector Loc = Actor->GetActorLocation();
	TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Result->SetObjectField(TEXT("location"), LocObj);

	return JsonToString(Result);
}

// ============================================================
// HandleEditorNotification — show a toast notification
// ============================================================

FString FBlueprintMCPServer::HandleEditorNotification(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString Message;
	if (!Json->TryGetStringField(TEXT("message"), Message) || Message.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'message'."));
	}

	FString SeverityStr;
	Json->TryGetStringField(TEXT("severity"), SeverityStr);

	double Duration = 5.0;
	Json->TryGetNumberField(TEXT("duration"), Duration);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: editor_notification('%s')"), *Message);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("editor_notification requires editor mode."));
	}

	SNotificationItem::ECompletionState CompletionState = SNotificationItem::CS_None;
	if (SeverityStr.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Success;
	}
	else if (SeverityStr.Equals(TEXT("fail"), ESearchCase::IgnoreCase) || SeverityStr.Equals(TEXT("error"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Fail;
	}
	else if (SeverityStr.Equals(TEXT("pending"), ESearchCase::IgnoreCase))
	{
		CompletionState = SNotificationItem::CS_Pending;
	}

	FNotificationInfo Info(FText::FromString(Message));
	Info.bFireAndForget = true;
	Info.ExpireDuration = Duration;
	Info.bUseLargeFont = false;

	TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
	if (Notification.IsValid() && CompletionState != SNotificationItem::CS_None)
	{
		Notification->SetCompletionState(CompletionState);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("message"), Message);
	Result->SetNumberField(TEXT("duration"), Duration);

	return JsonToString(Result);
}

// ============================================================
// HandleSaveAll — save all dirty packages
// ============================================================

FString FBlueprintMCPServer::HandleSaveAll(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: save_all()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("save_all requires editor mode."));
	}

	bool bPromptUserToSave = false;
	bool bSaveMapPackages = true;
	bool bSaveContentPackages = true;

	bool bSuccess = FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);

	return JsonToString(Result);
}

// ============================================================
// HandleGetDirtyPackages — list unsaved packages
// ============================================================

FString FBlueprintMCPServer::HandleGetDirtyPackages(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_dirty_packages()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("get_dirty_packages requires editor mode."));
	}

	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), DirtyPackages.Num());

	TArray<TSharedPtr<FJsonValue>> PackageArray;
	for (UPackage* Package : DirtyPackages)
	{
		if (!Package) continue;

		TSharedRef<FJsonObject> PkgObj = MakeShared<FJsonObject>();
		PkgObj->SetStringField(TEXT("name"), Package->GetName());
		PkgObj->SetStringField(TEXT("fileName"), Package->FileName.ToString());

		PackageArray.Add(MakeShared<FJsonValueObject>(PkgObj));
	}
	Result->SetArrayField(TEXT("packages"), PackageArray);

	return JsonToString(Result);
}
