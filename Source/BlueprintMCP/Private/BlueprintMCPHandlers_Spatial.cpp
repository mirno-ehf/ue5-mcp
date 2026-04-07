#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "CollisionQueryParams.h"
#include "Engine/EngineTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleRaycast — perform a line trace from point A to point B
// ============================================================

FString FBlueprintMCPServer::HandleRaycast(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	// Parse start point
	const TSharedPtr<FJsonObject>* StartObj = nullptr;
	if (!Json->TryGetObjectField(TEXT("start"), StartObj))
	{
		return MakeErrorJson(TEXT("Missing required field: 'start' (object with x, y, z)."));
	}

	double StartX = 0, StartY = 0, StartZ = 0;
	(*StartObj)->TryGetNumberField(TEXT("x"), StartX);
	(*StartObj)->TryGetNumberField(TEXT("y"), StartY);
	(*StartObj)->TryGetNumberField(TEXT("z"), StartZ);

	// Parse end point
	const TSharedPtr<FJsonObject>* EndObj = nullptr;
	if (!Json->TryGetObjectField(TEXT("end"), EndObj))
	{
		return MakeErrorJson(TEXT("Missing required field: 'end' (object with x, y, z)."));
	}

	double EndX = 0, EndY = 0, EndZ = 0;
	(*EndObj)->TryGetNumberField(TEXT("x"), EndX);
	(*EndObj)->TryGetNumberField(TEXT("y"), EndY);
	(*EndObj)->TryGetNumberField(TEXT("z"), EndZ);

	FVector Start(StartX, StartY, StartZ);
	FVector End(EndX, EndY, EndZ);

	// Optional: trace channel
	FString ChannelStr;
	Json->TryGetStringField(TEXT("channel"), ChannelStr);

	// Optional: trace complex geometry
	bool bTraceComplex = false;
	Json->TryGetBoolField(TEXT("traceComplex"), bTraceComplex);

	// Optional: multi-hit
	bool bMulti = false;
	Json->TryGetBoolField(TEXT("multi"), bMulti);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: raycast(start=(%.1f,%.1f,%.1f), end=(%.1f,%.1f,%.1f), multi=%s)"),
		Start.X, Start.Y, Start.Z, End.X, End.Y, End.Z, bMulti ? TEXT("true") : TEXT("false"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("raycast requires editor mode."));
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

	// Determine trace channel
	ECollisionChannel TraceChannel = ECC_Visibility;
	if (ChannelStr.Equals(TEXT("Camera"), ESearchCase::IgnoreCase))
	{
		TraceChannel = ECC_Camera;
	}
	else if (ChannelStr.Equals(TEXT("WorldStatic"), ESearchCase::IgnoreCase))
	{
		TraceChannel = ECC_WorldStatic;
	}
	else if (ChannelStr.Equals(TEXT("WorldDynamic"), ESearchCase::IgnoreCase))
	{
		TraceChannel = ECC_WorldDynamic;
	}
	else if (ChannelStr.Equals(TEXT("Pawn"), ESearchCase::IgnoreCase))
	{
		TraceChannel = ECC_Pawn;
	}
	else if (ChannelStr.Equals(TEXT("PhysicsBody"), ESearchCase::IgnoreCase))
	{
		TraceChannel = ECC_PhysicsBody;
	}

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = bTraceComplex;
	QueryParams.bReturnPhysicalMaterial = true;

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

	if (bMulti)
	{
		TArray<FHitResult> Hits;
		bool bHit = World->LineTraceMultiByChannel(Hits, Start, End, TraceChannel, QueryParams);

		Result->SetBoolField(TEXT("hit"), bHit);
		Result->SetNumberField(TEXT("hitCount"), Hits.Num());

		TArray<TSharedPtr<FJsonValue>> HitArray;
		for (const FHitResult& Hit : Hits)
		{
			TSharedRef<FJsonObject> HitObj = MakeShared<FJsonObject>();

			TSharedRef<FJsonObject> ImpactPoint = MakeShared<FJsonObject>();
			ImpactPoint->SetNumberField(TEXT("x"), Hit.ImpactPoint.X);
			ImpactPoint->SetNumberField(TEXT("y"), Hit.ImpactPoint.Y);
			ImpactPoint->SetNumberField(TEXT("z"), Hit.ImpactPoint.Z);
			HitObj->SetObjectField(TEXT("impactPoint"), ImpactPoint);

			TSharedRef<FJsonObject> ImpactNormal = MakeShared<FJsonObject>();
			ImpactNormal->SetNumberField(TEXT("x"), Hit.ImpactNormal.X);
			ImpactNormal->SetNumberField(TEXT("y"), Hit.ImpactNormal.Y);
			ImpactNormal->SetNumberField(TEXT("z"), Hit.ImpactNormal.Z);
			HitObj->SetObjectField(TEXT("impactNormal"), ImpactNormal);

			HitObj->SetNumberField(TEXT("distance"), Hit.Distance);

			AActor* HitActor = Hit.GetActor();
			if (HitActor)
			{
				HitObj->SetStringField(TEXT("actorLabel"), HitActor->GetActorLabel());
				HitObj->SetStringField(TEXT("actorClass"), HitActor->GetClass()->GetName());
			}

			if (Hit.GetComponent())
			{
				HitObj->SetStringField(TEXT("componentName"), Hit.GetComponent()->GetName());
			}

			if (Hit.PhysMaterial.IsValid())
			{
				HitObj->SetStringField(TEXT("physicalMaterial"), Hit.PhysMaterial->GetName());
			}

			HitArray.Add(MakeShared<FJsonValueObject>(HitObj));
		}
		Result->SetArrayField(TEXT("hits"), HitArray);
	}
	else
	{
		FHitResult Hit;
		bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, TraceChannel, QueryParams);

		Result->SetBoolField(TEXT("hit"), bHit);

		if (bHit)
		{
			TSharedRef<FJsonObject> ImpactPoint = MakeShared<FJsonObject>();
			ImpactPoint->SetNumberField(TEXT("x"), Hit.ImpactPoint.X);
			ImpactPoint->SetNumberField(TEXT("y"), Hit.ImpactPoint.Y);
			ImpactPoint->SetNumberField(TEXT("z"), Hit.ImpactPoint.Z);
			Result->SetObjectField(TEXT("impactPoint"), ImpactPoint);

			TSharedRef<FJsonObject> ImpactNormal = MakeShared<FJsonObject>();
			ImpactNormal->SetNumberField(TEXT("x"), Hit.ImpactNormal.X);
			ImpactNormal->SetNumberField(TEXT("y"), Hit.ImpactNormal.Y);
			ImpactNormal->SetNumberField(TEXT("z"), Hit.ImpactNormal.Z);
			Result->SetObjectField(TEXT("impactNormal"), ImpactNormal);

			Result->SetNumberField(TEXT("distance"), Hit.Distance);

			AActor* HitActor = Hit.GetActor();
			if (HitActor)
			{
				Result->SetStringField(TEXT("actorLabel"), HitActor->GetActorLabel());
				Result->SetStringField(TEXT("actorClass"), HitActor->GetClass()->GetName());
			}

			if (Hit.GetComponent())
			{
				Result->SetStringField(TEXT("componentName"), Hit.GetComponent()->GetName());
			}

			if (Hit.PhysMaterial.IsValid())
			{
				Result->SetStringField(TEXT("physicalMaterial"), Hit.PhysMaterial->GetName());
			}
		}
	}

	TSharedRef<FJsonObject> StartJson = MakeShared<FJsonObject>();
	StartJson->SetNumberField(TEXT("x"), Start.X);
	StartJson->SetNumberField(TEXT("y"), Start.Y);
	StartJson->SetNumberField(TEXT("z"), Start.Z);
	Result->SetObjectField(TEXT("traceStart"), StartJson);

	TSharedRef<FJsonObject> EndJson = MakeShared<FJsonObject>();
	EndJson->SetNumberField(TEXT("x"), End.X);
	EndJson->SetNumberField(TEXT("y"), End.Y);
	EndJson->SetNumberField(TEXT("z"), End.Z);
	Result->SetObjectField(TEXT("traceEnd"), EndJson);

	return JsonToString(Result);
}
