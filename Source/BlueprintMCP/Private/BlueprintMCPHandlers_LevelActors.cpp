#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "Selection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

static AActor* FindActorByLabel(const FString& Label, FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("Editor not available (running in commandlet mode?).");
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
			return Actor;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
			return Actor;
	}
	OutError = FString::Printf(TEXT("Actor with label '%s' not found in the current level. Use list_actors to see available actors."), *Label);
	return nullptr;
}

FString FBlueprintMCPServer::HandleAttachActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ChildLabel, ParentLabel;
	if (!Json->TryGetStringField(TEXT("childActor"), ChildLabel) || ChildLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'childActor' (actor label)."));
	if (!Json->TryGetStringField(TEXT("parentActor"), ParentLabel) || ParentLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'parentActor' (actor label)."));
	FString SocketName;
	Json->TryGetStringField(TEXT("socketName"), SocketName);
	FString AttachRuleStr;
	Json->TryGetStringField(TEXT("attachmentRule"), AttachRuleStr);
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: attach_actor(child='%s', parent='%s', socket='%s')"), *ChildLabel, *ParentLabel, *SocketName);
	if (!bIsEditor) return MakeErrorJson(TEXT("attach_actor requires editor mode."));
	FString Error;
	AActor* Child = FindActorByLabel(ChildLabel, Error);
	if (!Child) return MakeErrorJson(Error);
	AActor* Parent = FindActorByLabel(ParentLabel, Error);
	if (!Parent) return MakeErrorJson(Error);
	if (Child == Parent) return MakeErrorJson(TEXT("Cannot attach an actor to itself."));
	EAttachmentRule AttachRule = EAttachmentRule::KeepWorld;
	if (AttachRuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
		AttachRule = EAttachmentRule::KeepRelative;
	else if (AttachRuleStr.Equals(TEXT("SnapToTarget"), ESearchCase::IgnoreCase))
		AttachRule = EAttachmentRule::SnapToTarget;
	FName Socket = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);
	Child->AttachToActor(Parent, FAttachmentTransformRules(AttachRule, true), Socket);
	if (GEditor) GEditor->RedrawAllViewports(true);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("childActor"), ChildLabel);
	Result->SetStringField(TEXT("parentActor"), ParentLabel);
	Result->SetStringField(TEXT("attachmentRule"), AttachRuleStr.IsEmpty() ? TEXT("KeepWorld") : AttachRuleStr);
	if (!SocketName.IsEmpty()) Result->SetStringField(TEXT("socketName"), SocketName);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleDetachActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: detach_actor('%s')"), *ActorLabel);
	if (!bIsEditor) return MakeErrorJson(TEXT("detach_actor requires editor mode."));
	FString Error;
	AActor* Actor = FindActorByLabel(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);
	AActor* PreviousParent = Actor->GetAttachParentActor();
	if (!PreviousParent)
		return MakeErrorJson(FString::Printf(TEXT("Actor '%s' is not attached to any parent."), *ActorLabel));
	FString DetachRuleStr;
	Json->TryGetStringField(TEXT("detachmentRule"), DetachRuleStr);
	EDetachmentRule DetachRule = EDetachmentRule::KeepWorld;
	if (DetachRuleStr.Equals(TEXT("KeepRelative"), ESearchCase::IgnoreCase))
		DetachRule = EDetachmentRule::KeepRelative;
	Actor->DetachFromActor(FDetachmentTransformRules(DetachRule, true));
	if (GEditor) GEditor->RedrawAllViewports(true);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("previousParent"), PreviousParent->GetActorLabel());
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleDuplicateActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: duplicate_actor('%s')"), *ActorLabel);
	if (!bIsEditor) return MakeErrorJson(TEXT("duplicate_actor requires editor mode."));
	FString Error;
	AActor* SourceActor = FindActorByLabel(ActorLabel, Error);
	if (!SourceActor) return MakeErrorJson(Error);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	double OffsetX = 0, OffsetY = 0, OffsetZ = 0;
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (Json->TryGetObjectField(TEXT("offset"), OffsetObj))
	{
		(*OffsetObj)->TryGetNumberField(TEXT("x"), OffsetX);
		(*OffsetObj)->TryGetNumberField(TEXT("y"), OffsetY);
		(*OffsetObj)->TryGetNumberField(TEXT("z"), OffsetZ);
	}
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(SourceActor, true, true);
	GEditor->edactDuplicateSelected(World->GetCurrentLevel(), false);
	AActor* NewActor = nullptr;
	USelection* Selection = GEditor->GetSelectedActors();
	for (int32 i = 0; i < Selection->Num(); ++i)
	{
		AActor* Selected = Cast<AActor>(Selection->GetSelectedObject(i));
		if (Selected && Selected != SourceActor) { NewActor = Selected; break; }
	}
	if (!NewActor) return MakeErrorJson(TEXT("Duplication failed."));
	if (OffsetX != 0 || OffsetY != 0 || OffsetZ != 0)
		NewActor->SetActorLocation(NewActor->GetActorLocation() + FVector(OffsetX, OffsetY, OffsetZ));
	FString NewLabel;
	if (Json->TryGetStringField(TEXT("newLabel"), NewLabel) && !NewLabel.IsEmpty())
		NewActor->SetActorLabel(NewLabel);
	if (GEditor) GEditor->RedrawAllViewports(true);
	FVector Loc = NewActor->GetActorLocation();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("sourceActor"), ActorLabel);
	Result->SetStringField(TEXT("newActorLabel"), NewActor->GetActorLabel());
	Result->SetStringField(TEXT("newActorClass"), NewActor->GetClass()->GetName());
	TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Result->SetObjectField(TEXT("location"), LocObj);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleRenameActor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	FString NewLabel;
	if (!Json->TryGetStringField(TEXT("newLabel"), NewLabel) || NewLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'newLabel'."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: rename_actor('%s' -> '%s')"), *ActorLabel, *NewLabel);
	if (!bIsEditor) return MakeErrorJson(TEXT("rename_actor requires editor mode."));
	FString Error;
	AActor* Actor = FindActorByLabel(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);
	FString OldLabel = Actor->GetActorLabel();
	Actor->SetActorLabel(NewLabel);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("oldLabel"), OldLabel);
	Result->SetStringField(TEXT("newLabel"), Actor->GetActorLabel());
	Result->SetStringField(TEXT("actorClass"), Actor->GetClass()->GetName());
	return JsonToString(Result);
}
