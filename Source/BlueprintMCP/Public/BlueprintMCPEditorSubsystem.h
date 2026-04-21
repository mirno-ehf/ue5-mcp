#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Tickable.h"
#include "BlueprintMCPServer.h"
#include "BlueprintMCPEditorSubsystem.generated.h"

/**
 * Editor subsystem that hosts the Blueprint MCP HTTP server inside the running
 * UE5 editor. When active, the MCP TypeScript wrapper connects instantly
 * (no commandlet spawn, no extra RAM).
 *
 * Requests are dequeued and processed on the editor's game thread via
 * FTickableEditorObject::Tick().
 */
UCLASS()
class UBlueprintMCPEditorSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	// UEditorSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableEditorObject
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual TStatId GetStatId() const override;

private:
	void HandleAssetRegistryReady();

	TUniquePtr<FBlueprintMCPServer> Server;
	FDelegateHandle OnFilesLoadedHandle;
};
