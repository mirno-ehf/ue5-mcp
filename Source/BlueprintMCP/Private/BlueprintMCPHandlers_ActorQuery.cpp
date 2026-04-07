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

static AActor* FindActorByLabelQuery(UWorld* World, const FString& Label)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == Label) return Actor;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) return Actor;
	}
	return nullptr;
}

static TSharedRef<FJsonObject> ActorToJson(AActor* Actor)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
	Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	FVector Loc = Actor->GetActorLocation();
	TSharedRef<FJsonObject> LocObj = MakeShared<FJsonObject>();
	LocObj->SetNumberField(TEXT("x"), Loc.X);
	LocObj->SetNumberField(TEXT("y"), Loc.Y);
	LocObj->SetNumberField(TEXT("z"), Loc.Z);
	Obj->SetObjectField(TEXT("location"), LocObj);
	TArray<TSharedPtr<FJsonValue>> TagArr;
	for (const FName& Tag : Actor->Tags)
		TagArr.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	Obj->SetArrayField(TEXT("tags"), TagArr);
	return Obj;
}

FString FBlueprintMCPServer::HandleFindActorsByTag(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString Tag;
	if (!Json->TryGetStringField(TEXT("tag"), Tag) || Tag.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'tag'."));
	if (!bIsEditor) return MakeErrorJson(TEXT("find_actors_by_tag requires editor mode."));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: find_actors_by_tag('%s')"), *Tag);
	FName TagName(*Tag);
	TArray<TSharedPtr<FJsonValue>> Results;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->Tags.Contains(TagName))
			Results.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("tag"), Tag);
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetArrayField(TEXT("actors"), Results);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleFindActorsByClass(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ClassName;
	if (!Json->TryGetStringField(TEXT("className"), ClassName) || ClassName.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'className'."));
	if (!bIsEditor) return MakeErrorJson(TEXT("find_actors_by_class requires editor mode."));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: find_actors_by_class('%s')"), *ClassName);
	TArray<TSharedPtr<FJsonValue>> Results;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && (Actor->GetClass()->GetName() == ClassName ||
			Actor->GetClass()->GetName().Equals(ClassName, ESearchCase::IgnoreCase)))
			Results.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("className"), ClassName);
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetArrayField(TEXT("actors"), Results);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleFindActorsInRadius(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	double X = 0, Y = 0, Z = 0, Radius = 0;
	const TSharedPtr<FJsonObject>* OriginObj = nullptr;
	if (!Json->TryGetObjectField(TEXT("origin"), OriginObj))
		return MakeErrorJson(TEXT("Missing required field: 'origin' (object with x, y, z)."));
	(*OriginObj)->TryGetNumberField(TEXT("x"), X);
	(*OriginObj)->TryGetNumberField(TEXT("y"), Y);
	(*OriginObj)->TryGetNumberField(TEXT("z"), Z);
	if (!Json->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
		return MakeErrorJson(TEXT("Missing or invalid 'radius' (must be > 0)."));
	if (!bIsEditor) return MakeErrorJson(TEXT("find_actors_in_radius requires editor mode."));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: find_actors_in_radius(%.0f, %.0f, %.0f, r=%.0f)"), X, Y, Z, Radius);
	FVector Origin(X, Y, Z);
	double RadiusSq = Radius * Radius;
	TArray<TSharedPtr<FJsonValue>> Results;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && FVector::DistSquared(Actor->GetActorLocation(), Origin) <= RadiusSq)
		{
			TSharedRef<FJsonObject> Entry = ActorToJson(Actor);
			Entry->SetNumberField(TEXT("distance"), FVector::Dist(Actor->GetActorLocation(), Origin));
			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Results.Num());
	Result->SetNumberField(TEXT("radius"), Radius);
	Result->SetArrayField(TEXT("actors"), Results);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleGetActorBounds(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	if (!bIsEditor) return MakeErrorJson(TEXT("get_actor_bounds requires editor mode."));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	AActor* Actor = FindActorByLabelQuery(World, ActorLabel);
	if (!Actor) return MakeErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_actor_bounds('%s')"), *ActorLabel);
	FVector Origin, BoxExtent;
	Actor->GetActorBounds(false, Origin, BoxExtent);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	TSharedRef<FJsonObject> OriginObj = MakeShared<FJsonObject>();
	OriginObj->SetNumberField(TEXT("x"), Origin.X);
	OriginObj->SetNumberField(TEXT("y"), Origin.Y);
	OriginObj->SetNumberField(TEXT("z"), Origin.Z);
	Result->SetObjectField(TEXT("origin"), OriginObj);
	TSharedRef<FJsonObject> ExtentObj = MakeShared<FJsonObject>();
	ExtentObj->SetNumberField(TEXT("x"), BoxExtent.X);
	ExtentObj->SetNumberField(TEXT("y"), BoxExtent.Y);
	ExtentObj->SetNumberField(TEXT("z"), BoxExtent.Z);
	Result->SetObjectField(TEXT("boxExtent"), ExtentObj);
	return JsonToString(Result);
}

FString FBlueprintMCPServer::HandleSetActorTags(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid()) return MakeErrorJson(TEXT("Invalid JSON body."));
	FString ActorLabel;
	if (!Json->TryGetStringField(TEXT("actorLabel"), ActorLabel) || ActorLabel.IsEmpty())
		return MakeErrorJson(TEXT("Missing required field: 'actorLabel'."));
	const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
	if (!Json->TryGetArrayField(TEXT("tags"), TagsArr))
		return MakeErrorJson(TEXT("Missing required field: 'tags' (array of strings)."));
	if (!bIsEditor) return MakeErrorJson(TEXT("set_actor_tags requires editor mode."));
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return MakeErrorJson(TEXT("No editor world available."));
	AActor* Actor = FindActorByLabelQuery(World, ActorLabel);
	if (!Actor) return MakeErrorJson(FString::Printf(TEXT("Actor '%s' not found."), *ActorLabel));
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: set_actor_tags('%s', %d tags)"), *ActorLabel, TagsArr->Num());
	Actor->Tags.Empty();
	for (const TSharedPtr<FJsonValue>& Val : *TagsArr)
	{
		FString TagStr;
		if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
			Actor->Tags.Add(FName(*TagStr));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("actorLabel"), ActorLabel);
	Result->SetNumberField(TEXT("tagCount"), Actor->Tags.Num());
	TArray<TSharedPtr<FJsonValue>> OutTags;
	for (const FName& Tag : Actor->Tags)
		OutTags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	Result->SetArrayField(TEXT("tags"), OutTags);
	return JsonToString(Result);
}
