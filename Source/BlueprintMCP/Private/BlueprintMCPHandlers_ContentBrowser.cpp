#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleNavigateContentBrowser — navigate to a path in the Content Browser
// ============================================================

FString FBlueprintMCPServer::HandleNavigateContentBrowser(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString Path;
	if (!Json->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'path'. Provide a content path like '/Game/Blueprints'."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: navigate_content_browser('%s')"), *Path);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("navigate_content_browser requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	IContentBrowserSingleton& ContentBrowser = ContentBrowserModule.Get();

	// Navigate to the specified path
	ContentBrowser.FocusPrimaryContentBrowser(false);

	TArray<FString> PathArray;
	PathArray.Add(Path);
	ContentBrowser.SetSelectedPaths(PathArray, false);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("navigatedTo"), Path);

	return JsonToString(Result);
}

// ============================================================
// HandleOpenAssetEditor — open an asset in its appropriate editor
// ============================================================

FString FBlueprintMCPServer::HandleOpenAssetEditor(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString AssetPath;
	if (!Json->TryGetStringField(TEXT("assetPath"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'assetPath'. Provide the full asset path or name."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: open_asset_editor('%s')"), *AssetPath);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("open_asset_editor requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	// Try to find the asset
	FAssetData* AssetData = FindAnyAsset(AssetPath);
	if (!AssetData)
	{
		return MakeErrorJson(FString::Printf(TEXT("Asset '%s' not found. Use list_blueprints or search to find available assets."), *AssetPath));
	}

	UObject* Asset = AssetData->GetAsset();
	if (!Asset)
	{
		return MakeErrorJson(FString::Printf(TEXT("Failed to load asset '%s'."), *AssetPath));
	}

	// Open the asset editor
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!AssetEditorSubsystem)
	{
		return MakeErrorJson(TEXT("AssetEditorSubsystem not available."));
	}

	bool bOpened = AssetEditorSubsystem->OpenEditorForAsset(Asset);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bOpened);
	Result->SetStringField(TEXT("assetName"), AssetData->AssetName.ToString());
	Result->SetStringField(TEXT("assetPath"), AssetData->PackageName.ToString());
	Result->SetStringField(TEXT("assetClass"), AssetData->AssetClassPath.GetAssetName().ToString());

	if (!bOpened)
	{
		Result->SetStringField(TEXT("warning"), TEXT("Asset editor could not be opened. The asset type may not have a dedicated editor."));
	}

	return JsonToString(Result);
}
