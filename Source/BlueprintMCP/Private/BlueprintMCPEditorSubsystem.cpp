#include "BlueprintMCPEditorSubsystem.h"
#include "BlueprintMCPServer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

void UBlueprintMCPEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Don't start in commandlet mode — the commandlet has its own server instance.
	if (IsRunningCommandlet())
	{
		return;
	}

	Server = MakeUnique<FBlueprintMCPServer>();
	if (Server->Start(9847, /*bEditorMode=*/true))
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Editor subsystem started — MCP server on port %d"), Server->GetPort());

		// Asset Registry loads asynchronously during editor startup.
		// The initial scan in Start() only sees engine assets.
		// Defer a full rescan until the registry finishes gathering.
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AR = ARM.Get();

		if (AR.IsGathering())
		{
			OnFilesLoadedHandle = AR.OnFilesLoaded().AddUObject(
				this, &UBlueprintMCPEditorSubsystem::HandleAssetRegistryReady);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Editor subsystem failed to start MCP server (port may be in use)"));
		Server.Reset();
	}
}

void UBlueprintMCPEditorSubsystem::HandleAssetRegistryReady()
{
	if (OnFilesLoadedHandle.IsValid())
	{
		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().OnFilesLoaded().Remove(OnFilesLoadedHandle);
		OnFilesLoadedHandle.Reset();
	}

	if (Server && Server->IsRunning())
	{
		Server->HandleRescan();
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Deferred rescan complete after Asset Registry finished gathering."));
	}
}

void UBlueprintMCPEditorSubsystem::Deinitialize()
{
	if (OnFilesLoadedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
	{
		FAssetRegistryModule& ARM = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		ARM.Get().OnFilesLoaded().Remove(OnFilesLoadedHandle);
		OnFilesLoadedHandle.Reset();
	}

	if (Server)
	{
		Server->Stop();
		Server.Reset();
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Editor subsystem stopped."));
	}

	Super::Deinitialize();
}

void UBlueprintMCPEditorSubsystem::Tick(float DeltaTime)
{
	if (Server)
	{
		Server->ProcessOneRequest();
	}
}

bool UBlueprintMCPEditorSubsystem::IsTickable() const
{
	return Server.IsValid() && Server->IsRunning();
}

TStatId UBlueprintMCPEditorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBlueprintMCPEditorSubsystem, STATGROUP_Tickables);
}
