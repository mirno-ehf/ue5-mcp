#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetData.h"
#include "HttpResultCallback.h"
#include "EdGraph/EdGraphPin.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UBlueprint;
class UMaterial;
class UMaterialInstanceConstant;
class UMaterialFunction;
class UMaterialExpression;

struct FPinConnectionRecord
{
	FString SourceNodeGuid;
	FString SourcePinName;
	FString TargetNodeGuid;
	FString TargetPinName;
};

struct FNodeRecord
{
	FString NodeGuid;
	FString NodeClass;
	FString NodeTitle;
	FString StructType;
};

struct FGraphSnapshotData
{
	TArray<FNodeRecord> Nodes;
	TArray<FPinConnectionRecord> Connections;
};

struct FGraphSnapshot
{
	FString SnapshotId;
	FString BlueprintName;
	FString BlueprintPath;
	FDateTime CreatedAt;
	TMap<FString, FGraphSnapshotData> Graphs;
};

class FBlueprintMCPServer
{
public:
	bool Start(int32 InPort, bool bEditorMode = false);
	void Stop();
	bool ProcessOneRequest();
	bool IsRunning() const { return bRunning; }

	/** Re-scan the Asset Registry and refresh cached asset lists. */
	FString HandleRescan();

	/** Port the server is listening on. */
	int32 GetPort() const { return Port; }
	int32 GetBlueprintCount() const { return AllBlueprintAssets.Num(); }
	int32 GetMapCount() const { return AllMapAssets.Num(); }
	int32 GetMaterialCount() const { return AllMaterialAssets.Num(); }
	int32 GetMaterialInstanceCount() const { return AllMaterialInstanceAssets.Num(); }

private:
	using FRequestHandler = TFunction<FString(const TMap<FString, FString>&, const FString&)>;
	TMap<FString, FRequestHandler> HandlerMap;
	TSet<FString> MutationEndpoints;
	void RegisterHandlers();
	struct FPendingRequest
	{
		FString Endpoint;
		TMap<FString, FString> QueryParams;
		FString Body;
		FHttpResultCallback OnComplete;
	};

	TQueue<TSharedPtr<FPendingRequest>> RequestQueue;
	TArray<FAssetData> AllBlueprintAssets;
	TArray<FAssetData> AllMapAssets;
	TArray<FAssetData> AllMaterialAssets;
	TArray<FAssetData> AllMaterialInstanceAssets;
	TArray<FAssetData> AllMaterialFunctionAssets;
	int32 Port = 9847;
	bool bRunning = false;
	bool bIsEditor = false;

	// ----- Request handlers (read-only) -----
	FString HandleList(const TMap<FString, FString>& Params);
	FString HandleGetBlueprint(const TMap<FString, FString>& Params);
	FString HandleGetGraph(const TMap<FString, FString>& Params);
	FString HandleSearch(const TMap<FString, FString>& Params);
	FString HandleFindReferences(const TMap<FString, FString>& Params);
	FString HandleSearchByType(const TMap<FString, FString>& Params);
	FString HandleReplaceFunctionCalls(const FString& Body);
	FString HandleChangeVariableType(const FString& Body);
	FString HandleChangeFunctionParamType(const FString& Body);
	FString HandleRemoveFunctionParameter(const FString& Body);
	FString HandleDeleteAsset(const FString& Body);
	FString HandleDeleteNode(const FString& Body);
	FString HandleDuplicateNodes(const FString& Body);
	FString HandleAddNode(const FString& Body);
	FString HandleRenameAsset(const FString& Body);
	FString HandleValidateBlueprint(const FString& Body);
	FString HandleValidateAllBlueprints(const FString& Body);
	FString HandleConnectPins(const FString& Body);
	FString HandleDisconnectPin(const FString& Body);
	FString HandleRefreshAllNodes(const FString& Body);
	FString HandleSetPinDefault(const FString& Body);
	FString HandleMoveNode(const FString& Body);
	FString HandleGetNodeComment(const FString& Body);
	FString HandleSetNodeComment(const FString& Body);
	FString HandleGetPinInfo(const FString& Body);
	FString HandleCheckPinCompatibility(const FString& Body);
	FString HandleListClasses(const FString& Body);
	FString HandleListFunctions(const FString& Body);
	FString HandleListProperties(const FString& Body);
	FString HandleChangeStructNodeType(const FString& Body);
	FString HandleReparentBlueprint(const FString& Body);
	FString HandleCreateBlueprint(const FString& Body);
	FString HandleCreateGraph(const FString& Body);
	FString HandleCreateStruct(const FString& Body);
	FString HandleCreateEnum(const FString& Body);
	FString HandleAddStructProperty(const FString& Body);
	FString HandleRemoveStructProperty(const FString& Body);
	FString HandleDeleteGraph(const FString& Body);
	FString HandleRenameGraph(const FString& Body);
	FString HandleAddVariable(const FString& Body);
	FString HandleRemoveVariable(const FString& Body);
	FString HandleSetVariableMetadata(const FString& Body);
	FString HandleAddInterface(const FString& Body);
	FString HandleRemoveInterface(const FString& Body);
	FString HandleListInterfaces(const FString& Body);
	FString HandleAddEventDispatcher(const FString& Body);
	FString HandleListEventDispatchers(const FString& Body);
	FString HandleAddFunctionParameter(const FString& Body);
	FString HandleAddComponent(const FString& Body);
	FString HandleRemoveComponent(const FString& Body);
	FString HandleListComponents(const FString& Body);
	FString HandleSetBlueprintDefault(const FString& Body);
	FString HandleTestSave(const TMap<FString, FString>& Params);
	FString HandleSnapshotGraph(const FString& Body);
	FString HandleDiffGraph(const FString& Body);
	FString HandleRestoreGraph(const FString& Body);
	FString HandleFindDisconnectedPins(const FString& Body);
	FString HandleAnalyzeRebuildImpact(const FString& Body);
	FString HandleDiffBlueprints(const FString& Body);
	FString HandleListMaterials(const TMap<FString, FString>& Params);
	FString HandleGetMaterial(const TMap<FString, FString>& Params);
	FString HandleGetMaterialGraph(const TMap<FString, FString>& Params);
	FString HandleDescribeMaterial(const FString& Body);
	FString HandleSearchMaterials(const TMap<FString, FString>& Params);
	FString HandleFindMaterialReferences(const FString& Body);
	FString HandleCreateMaterial(const FString& Body);
	FString HandleSetMaterialProperty(const FString& Body);
	FString HandleAddMaterialExpression(const FString& Body);
	FString HandleDeleteMaterialExpression(const FString& Body);
	FString HandleConnectMaterialPins(const FString& Body);
	FString HandleDisconnectMaterialPin(const FString& Body);
	FString HandleSetExpressionValue(const FString& Body);
	FString HandleMoveMaterialExpression(const FString& Body);
	FString HandleCreateMaterialInstance(const FString& Body);
	FString HandleSetMaterialInstanceParameter(const FString& Body);
	FString HandleGetMaterialInstanceParameters(const TMap<FString, FString>& Params);
	FString HandleReparentMaterialInstance(const FString& Body);
	FString HandleListMaterialFunctions(const TMap<FString, FString>& Params);
	FString HandleGetMaterialFunction(const TMap<FString, FString>& Params);
	FString HandleCreateMaterialFunction(const FString& Body);
	FString HandleValidateMaterial(const FString& Body);
	FString HandleSnapshotMaterialGraph(const FString& Body);
	FString HandleDiffMaterialGraph(const FString& Body);
	FString HandleRestoreMaterialGraph(const FString& Body);
	FString HandleExecCommand(const FString& Body);


	// ----- Level actor tools -----
	FString HandleAttachActor(const FString& Body);
	FString HandleDetachActor(const FString& Body);
	FString HandleDuplicateActor(const FString& Body);
	FString HandleRenameActor(const FString& Body);

	// ----- Actor query tools -----
	FString HandleFindActorsByTag(const FString& Body);
	FString HandleFindActorsByClass(const FString& Body);
	FString HandleFindActorsInRadius(const FString& Body);
	FString HandleGetActorBounds(const FString& Body);
	FString HandleSetActorTags(const FString& Body);

	// ----- Spatial tools -----
	FString HandleRaycast(const FString& Body);

	// ----- Camera tools -----
	FString HandleGetViewportCamera(const FString& Body);
	FString HandleSetViewportCamera(const FString& Body);
	// ----- View mode tools -----
	FString HandleSetViewMode(const FString& Body);
	FString HandleSetShowFlags(const FString& Body);
	FString HandleSetViewportType(const FString& Body);
	FString HandleSetRealtimeRendering(const FString& Body);
	FString HandleSetGameView(const FString& Body);
	// ----- PIE runtime tools -----
	FString HandlePIEGetPlayerTransform(const FString& Body);
	FString HandlePIETeleportPlayer(const FString& Body);
	FString HandlePIEQueryActors(const FString& Body);
	// ----- Sublevel tools -----
	FString HandleGetLevelInfo(const FString& Body);
	FString HandleListSublevels(const FString& Body);
	FString HandleLoadSublevel(const FString& Body);
	FString HandleUnloadSublevel(const FString& Body);
	// ----- Editor utility tools -----
	FString HandleFocusActor(const FString& Body);
	FString HandleEditorNotification(const FString& Body);
	FString HandleSaveAll(const FString& Body);
	FString HandleGetDirtyPackages(const FString& Body);
	// ----- Selection tools -----
	FString HandleGetEditorSelection(const FString& Body);
	FString HandleSetEditorSelection(const FString& Body);
	FString HandleClearSelection(const FString& Body);
	// ----- CVar tools -----
	FString HandleGetCVar(const FString& Body);
	FString HandleSetCVar(const FString& Body);
	FString HandleListCVars(const FString& Body);
	// ----- Output log tools -----
	FString HandleGetOutputLog(const FString& Body);
	FString HandleClearOutputLog(const FString& Body);
	// ----- Screenshot tools -----
	FString HandleTakeScreenshot(const FString& Body);
	FString HandleTakeHighResScreenshot(const FString& Body);

	// ----- Animation Blueprint handlers -----
	FString HandleCreateAnimBlueprint(const FString& Body);
	FString HandleAddAnimState(const FString& Body);
	FString HandleRemoveAnimState(const FString& Body);
	FString HandleAddAnimTransition(const FString& Body);
	FString HandleSetTransitionRule(const FString& Body);
	FString HandleAddAnimNode(const FString& Body);
	FString HandleAddStateMachine(const FString& Body);
	FString HandleSetStateAnimation(const FString& Body);
	FString HandleListAnimSlots(const FString& Body);
	FString HandleListSyncGroups(const FString& Body);
	FString HandleCreateBlendSpace(const FString& Body);
	FString HandleSetBlendSpaceSamples(const FString& Body);
	FString HandleSetStateBlendSpace(const FString& Body);

	TSharedRef<FJsonObject> SerializeBlueprint(UBlueprint* BP);
	TSharedPtr<FJsonObject> SerializeGraph(UEdGraph* Graph);
	TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node);
	TSharedPtr<FJsonObject> SerializePin(UEdGraphPin* Pin);
	TSharedPtr<FJsonObject> SerializeMaterialExpression(UMaterialExpression* Expression);
	FString JsonToString(TSharedRef<FJsonObject> JsonObj);

	FAssetData* FindAnyAsset(const FString& NameOrPath);
	FAssetData* FindBlueprintAsset(const FString& NameOrPath);
	FAssetData* FindMapAsset(const FString& NameOrPath);
	UBlueprint* LoadBlueprintByName(const FString& NameOrPath, FString& OutError);
	UEdGraphNode* FindNodeByGuid(UBlueprint* BP, const FString& GuidString, UEdGraph** OutGraph = nullptr);
	TSharedPtr<FJsonObject> ParseBodyJson(const FString& Body);
	FString MakeErrorJson(const FString& Message);
	bool SaveBlueprintPackage(UBlueprint* BP);
	static FString UrlDecode(const FString& EncodedString);

	void EnsureMaterialGraph(UMaterial* Material);
	FAssetData* FindMaterialAsset(const FString& NameOrPath);
	UMaterial* LoadMaterialByName(const FString& NameOrPath, FString& OutError);
	FAssetData* FindMaterialInstanceAsset(const FString& NameOrPath);
	UMaterialInstanceConstant* LoadMaterialInstanceByName(const FString& NameOrPath, FString& OutError);
	FAssetData* FindMaterialFunctionAsset(const FString& NameOrPath);
	UMaterialFunction* LoadMaterialFunctionByName(const FString& NameOrPath, FString& OutError);
	bool SaveMaterialPackage(UMaterial* Material);
	bool SaveGenericPackage(UObject* Asset);

	bool ResolveTypeFromString(const FString& TypeName, FEdGraphPinType& OutPinType, FString& OutError);
	static UClass* FindClassByName(const FString& ClassName);

	TMap<FString, FGraphSnapshot> Snapshots;
	TMap<FString, FGraphSnapshot> MaterialSnapshots;
	static const int32 MaxSnapshots = 50;
	FString GenerateSnapshotId(const FString& BlueprintName);
	FGraphSnapshotData CaptureGraphSnapshot(UEdGraph* Graph);
	void PruneOldSnapshots();
	bool SaveSnapshotToDisk(const FString& SnapshotId, const FGraphSnapshot& Snapshot);
	bool LoadSnapshotFromDisk(const FString& SnapshotId, FGraphSnapshot& OutSnapshot);
};
