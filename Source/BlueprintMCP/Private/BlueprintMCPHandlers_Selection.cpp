#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleGetEditorSelection — get currently selected actors
// ============================================================

FString FBlueprintMCPServer::HandleGetEditorSelection(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_editor_selection()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("get_editor_selection requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	USelection* Selection = GEditor->GetSelectedActors();
	if (!Selection)
	{
		return MakeErrorJson(TEXT("Selection object not available."));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Selection->Num());

	TArray<TSharedPtr<FJsonValue>> ActorArray;
	for (int32 i = 0; i < Selection->Num(); ++i)
	{
		AActor* Actor = Cast<AActor>(Selection->GetSelectedObject(i));
		if (!Actor) continue;

		TSharedRef<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		ActorObj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());

		FVector Loc = Actor->GetActorLocation();
		TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
		LocObj->SetNumberField(TEXT("x"), Loc.X);
		LocObj->SetNumberField(TEXT("y"), Loc.Y);
		LocObj->SetNumberField(TEXT("z"), Loc.Z);
		ActorObj->SetObjectField(TEXT("location"), LocObj);

		ActorArray.Add(MakeShared<FJsonValueObject>(ActorObj));
	}
	Result->SetArrayField(TEXT("selectedActors"), ActorArray);

	return JsonToString(Result);
}

// ============================================================
// HandleSetEditorSelection — select actors by label
// ============================================================

FString FBlueprintMCPServer::HandleSetEditorSelection(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorLabelsArray = nullptr;
	if (!Json->TryGetArrayField(TEXT("actorLabels"), ActorLabelsArray) || !ActorLabelsArray)
	{
		return MakeErrorJson(TEXT("Missing required field: 'actorLabels' (array of actor label strings)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: set_editor_selection(%d actors)"), ActorLabelsArray->Num());

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("set_editor_selection requires editor mode."));
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

	// Clear current selection
	GEditor->SelectNone(false, true, false);

	TArray<FString> Selected;
	TArray<FString> NotFound;

	for (const TSharedPtr<FJsonValue>& Value : *ActorLabelsArray)
	{
		FString Label = Value->AsString();
		if (Label.IsEmpty()) continue;

		bool bFound = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetActorLabel() == Label ||
				Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)))
			{
				GEditor->SelectActor(Actor, true, true);
				Selected.Add(Actor->GetActorLabel());
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			NotFound.Add(Label);
		}
	}

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("selectedCount"), Selected.Num());
	Result->SetNumberField(TEXT("notFoundCount"), NotFound.Num());

	TArray<TSharedPtr<FJsonValue>> SelectedArray;
	for (const FString& Label : Selected)
	{
		SelectedArray.Add(MakeShared<FJsonValueString>(Label));
	}
	Result->SetArrayField(TEXT("selected"), SelectedArray);

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArray;
		for (const FString& Label : NotFound)
		{
			NotFoundArray.Add(MakeShared<FJsonValueString>(Label));
		}
		Result->SetArrayField(TEXT("notFound"), NotFoundArray);
	}

	return JsonToString(Result);
}

// ============================================================
// HandleClearSelection — deselect all actors
// ============================================================

FString FBlueprintMCPServer::HandleClearSelection(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: clear_selection()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("clear_selection requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	int32 PreviousCount = GEditor->GetSelectedActors()->Num();
	GEditor->SelectNone(false, true, false);

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("previousSelectionCount"), PreviousCount);

	return JsonToString(Result);
}
