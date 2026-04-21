#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "HighResScreenshot.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleTakeScreenshot — capture a viewport screenshot
// ============================================================

FString FBlueprintMCPServer::HandleTakeScreenshot(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: take_screenshot()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("take_screenshot requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	FString Filename;
	if (!Json->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("Screenshot_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	// Ensure .png extension
	if (!Filename.EndsWith(TEXT(".png")))
	{
		Filename += TEXT(".png");
	}

	// Output directory
	FString OutputDir = FPaths::ProjectSavedDir() / TEXT("Screenshots");
	FString FullPath = OutputDir / Filename;

	// Get the active viewport
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}

	if (!ViewportClient || !ViewportClient->Viewport)
	{
		return MakeErrorJson(TEXT("No active viewport found."));
	}

	FViewport* Viewport = ViewportClient->Viewport;

	// Read pixels from viewport
	TArray<FColor> Bitmap;
	int32 Width = Viewport->GetSizeXY().X;
	int32 Height = Viewport->GetSizeXY().Y;

	if (Width <= 0 || Height <= 0)
	{
		return MakeErrorJson(TEXT("Viewport has invalid dimensions."));
	}

	bool bReadSuccess = Viewport->ReadPixels(Bitmap);
	if (!bReadSuccess || Bitmap.Num() == 0)
	{
		return MakeErrorJson(TEXT("Failed to read pixels from viewport."));
	}

	// Save as PNG
	TArray<uint8> PngData;
	FImageUtils::PNGCompressImageArray(Width, Height, Bitmap, PngData);

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*OutputDir);

	bool bSaved = FFileHelper::SaveArrayToFile(PngData, *FullPath);
	if (!bSaved)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to save screenshot to '%s'."), *FullPath));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("filename"), Filename);
	Result->SetStringField(TEXT("fullPath"), FullPath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Screenshot saved to '%s' (%dx%d)"), *FullPath, Width, Height);

	return JsonToString(Result);
}

// ============================================================
// HandleTakeHighResScreenshot — capture a high-resolution screenshot
// ============================================================

FString FBlueprintMCPServer::HandleTakeHighResScreenshot(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: take_high_res_screenshot()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("take_high_res_screenshot requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	double ResMultiplier = 2.0;
	Json->TryGetNumberField(TEXT("resolutionMultiplier"), ResMultiplier);
	if (ResMultiplier < 1.0) ResMultiplier = 1.0;
	if (ResMultiplier > 8.0) ResMultiplier = 8.0;

	FString Filename;
	if (!Json->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("HighRes_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	if (!Filename.EndsWith(TEXT(".png")))
	{
		Filename += TEXT(".png");
	}

	FString OutputDir = FPaths::ProjectSavedDir() / TEXT("Screenshots");
	FString FullPath = OutputDir / Filename;

	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}

	if (!ViewportClient || !ViewportClient->Viewport)
	{
		return MakeErrorJson(TEXT("No active viewport found."));
	}

	// Configure high-res screenshot settings
	FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
	Config.SetResolution(
		ViewportClient->Viewport->GetSizeXY().X,
		ViewportClient->Viewport->GetSizeXY().Y,
		ResMultiplier
	);
	Config.SetFilename(FullPath);
	Config.bMaskEnabled = false;

	// Request the screenshot
	ViewportClient->Viewport->TakeHighResScreenShot();

	int32 FinalWidth = FMath::CeilToInt(ViewportClient->Viewport->GetSizeXY().X * ResMultiplier);
	int32 FinalHeight = FMath::CeilToInt(ViewportClient->Viewport->GetSizeXY().Y * ResMultiplier);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("filename"), Filename);
	Result->SetStringField(TEXT("fullPath"), FullPath);
	Result->SetNumberField(TEXT("resolutionMultiplier"), ResMultiplier);
	Result->SetNumberField(TEXT("estimatedWidth"), FinalWidth);
	Result->SetNumberField(TEXT("estimatedHeight"), FinalHeight);
	Result->SetStringField(TEXT("note"), TEXT("High-res screenshot is captured asynchronously. The file may take a moment to appear on disk."));

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: High-res screenshot requested at %dx multiplier -> '%s'"), (int32)ResMultiplier, *FullPath);

	return JsonToString(Result);
}
