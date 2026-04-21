#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Helper — find an actor by label in the editor world
// (Shared with BlueprintMCPHandlers_LevelActors.cpp — uses same logic)
// ============================================================

static AActor* FindActorByLabelState(const FString& Label, FString& OutError)
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
		{
			return Actor;
		}
	}

	// Case-insensitive fallback
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
		{
			return Actor;
		}
	}

	OutError = FString::Printf(TEXT("Actor with label '%s' not found in the current level. Use list_actors to see available actors."), *Label);
	return nullptr;
}

// ============================================================
// HandleSetActorMobility — set an actor's mobility
// ============================================================

FString FBlueprintMCPServer::HandleSetActorMobility(const FString& Body)
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

	FString MobilityStr;
	if (!Json->TryGetStringField(TEXT("mobility"), MobilityStr) || MobilityStr.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'mobility' (Static, Stationary, or Movable)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: set_actor_mobility('%s', '%s')"), *ActorLabel, *MobilityStr);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("set_actor_mobility requires editor mode."));
	}

	FString Error;
	AActor* Actor = FindActorByLabelState(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);

	// Parse mobility
	EComponentMobility::Type NewMobility;
	if (MobilityStr.Equals(TEXT("Static"), ESearchCase::IgnoreCase))
	{
		NewMobility = EComponentMobility::Static;
	}
	else if (MobilityStr.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase))
	{
		NewMobility = EComponentMobility::Stationary;
	}
	else if (MobilityStr.Equals(TEXT("Movable"), ESearchCase::IgnoreCase))
	{
		NewMobility = EComponentMobility::Movable;
	}
	else
	{
		return MakeErrorJson(FString::Printf(TEXT("Invalid mobility value '%s'. Must be Static, Stationary, or Movable."), *MobilityStr));
	}

	USceneComponent* RootComp = Actor->GetRootComponent();
	if (!RootComp)
	{
		return MakeErrorJson(FString::Printf(TEXT("Actor '%s' has no root component to set mobility on."), *ActorLabel));
	}

	EComponentMobility::Type OldMobility = RootComp->Mobility;
	RootComp->SetMobility(NewMobility);
	Actor->MarkPackageDirty();

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	auto MobilityToString = [](EComponentMobility::Type M) -> FString {
		switch (M)
		{
		case EComponentMobility::Static:     return TEXT("Static");
		case EComponentMobility::Stationary: return TEXT("Stationary");
		case EComponentMobility::Movable:    return TEXT("Movable");
		default:                             return TEXT("Unknown");
		}
	};

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetStringField(TEXT("previousMobility"), MobilityToString(OldMobility));
	Result->SetStringField(TEXT("newMobility"), MobilityToString(NewMobility));

	return JsonToString(Result);
}

// ============================================================
// HandleSetActorVisibility — show/hide an actor
// ============================================================

FString FBlueprintMCPServer::HandleSetActorVisibility(const FString& Body)
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

	bool bVisible = true;
	if (!Json->TryGetBoolField(TEXT("visible"), bVisible))
	{
		return MakeErrorJson(TEXT("Missing required field: 'visible' (true or false)."));
	}

	bool bPropagateToChildren = true;
	Json->TryGetBoolField(TEXT("propagateToChildren"), bPropagateToChildren);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: set_actor_visibility('%s', visible=%s)"),
		*ActorLabel, bVisible ? TEXT("true") : TEXT("false"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("set_actor_visibility requires editor mode."));
	}

	FString Error;
	AActor* Actor = FindActorByLabelState(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);

	bool bWasHidden = Actor->IsHidden();
	Actor->SetIsTemporarilyHiddenInEditor(!bVisible);
	Actor->SetActorHiddenInGame(!bVisible);
	Actor->MarkPackageDirty();

	if (bPropagateToChildren)
	{
		TArray<AActor*> AttachedActors;
		Actor->GetAttachedActors(AttachedActors);
		for (AActor* Child : AttachedActors)
		{
			if (Child)
			{
				Child->SetIsTemporarilyHiddenInEditor(!bVisible);
				Child->SetActorHiddenInGame(!bVisible);
				Child->MarkPackageDirty();
			}
		}
	}

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetBoolField(TEXT("visible"), bVisible);
	Result->SetBoolField(TEXT("wasHidden"), bWasHidden);
	Result->SetBoolField(TEXT("propagatedToChildren"), bPropagateToChildren);

	return JsonToString(Result);
}

// ============================================================
// HandleSetActorPhysics — enable/disable physics simulation
// ============================================================

FString FBlueprintMCPServer::HandleSetActorPhysics(const FString& Body)
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

	bool bSimulatePhysics = true;
	if (!Json->TryGetBoolField(TEXT("simulatePhysics"), bSimulatePhysics))
	{
		return MakeErrorJson(TEXT("Missing required field: 'simulatePhysics' (true or false)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: set_actor_physics('%s', simulate=%s)"),
		*ActorLabel, bSimulatePhysics ? TEXT("true") : TEXT("false"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("set_actor_physics requires editor mode."));
	}

	FString Error;
	AActor* Actor = FindActorByLabelState(ActorLabel, Error);
	if (!Actor) return MakeErrorJson(Error);

	UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Actor->GetRootComponent());
	if (!PrimComp)
	{
		// Try to find any primitive component on the actor
		PrimComp = Actor->FindComponentByClass<UPrimitiveComponent>();
	}

	if (!PrimComp)
	{
		return MakeErrorJson(FString::Printf(TEXT("Actor '%s' has no primitive component that can simulate physics."), *ActorLabel));
	}

	bool bWasSimulating = PrimComp->IsSimulatingPhysics();

	// If enabling physics, ensure mobility is Movable
	if (bSimulatePhysics && PrimComp->Mobility != EComponentMobility::Movable)
	{
		PrimComp->SetMobility(EComponentMobility::Movable);
	}

	PrimComp->SetSimulatePhysics(bSimulatePhysics);

	bool bEnableGravity = true;
	if (Json->TryGetBoolField(TEXT("enableGravity"), bEnableGravity))
	{
		PrimComp->SetEnableGravity(bEnableGravity);
	}

	Actor->MarkPackageDirty();

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetBoolField(TEXT("simulatePhysics"), bSimulatePhysics);
	Result->SetBoolField(TEXT("wasSimulating"), bWasSimulating);
	Result->SetStringField(TEXT("component"), PrimComp->GetName());

	return JsonToString(Result);
}
