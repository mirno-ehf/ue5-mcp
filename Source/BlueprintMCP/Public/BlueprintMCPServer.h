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

// ----- Snapshot data structures -----

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
	FString StructType; // for Break/Make nodes
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
	TMap<FString, FGraphSnapshotData> Graphs; // graphName -> data
};

/**
 * FBlueprintMCPServer — plain C++ class (not a UCLASS) that owns all HTTP
 * serving logic for the Blueprint MCP protocol.
 *
 * Both the standalone commandlet (UBlueprintMCPCommandlet) and the in-editor
 * subsystem (UBlueprintMCPEditorSubsystem) delegate to an instance of this
 * class. The only difference is *who ticks the engine*:
 *   - Commandlet: manual FTSTicker loop
 *   - Editor subsystem: UE editor tick via FTickableEditorObject
 */
class FBlueprintMCPServer
{
public:
	/** Scan asset registry, bind HTTP routes, start listener on the given port.
	 *  Set bEditorMode=true when hosted inside the UE5 editor (disables /api/shutdown). */
	bool Start(int32 InPort, bool bEditorMode = false);

	/** Stop the HTTP listener and clean up. */
	void Stop();

	/**
	 * Dequeue and handle ONE pending HTTP request on the calling (game) thread.
	 * Call this every tick from whichever host owns this server.
	 * Returns true if a request was processed.
	 */
	bool ProcessOneRequest();

	/** Whether the HTTP server is currently listening. */
	bool IsRunning() const { return bRunning; }

	/** Port the server is listening on. */
	int32 GetPort() const { return Port; }

	/** Number of indexed Blueprint assets. */
	int32 GetBlueprintCount() const { return AllBlueprintAssets.Num(); }

	/** Number of indexed Map assets. */
	int32 GetMapCount() const { return AllMapAssets.Num(); }

	/** Number of indexed Material assets. */
	int32 GetMaterialCount() const { return AllMaterialAssets.Num(); }

	/** Number of indexed Material Instance assets. */
	int32 GetMaterialInstanceCount() const { return AllMaterialInstanceAssets.Num(); }

private:
	// ----- TMap-based request dispatch -----
	using FRequestHandler = TFunction<FString(const TMap<FString, FString>&, const FString&)>;
	TMap<FString, FRequestHandler> HandlerMap;
	TSet<FString> MutationEndpoints;
	void RegisterHandlers();
	// ----- Queued request model -----
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

	// ----- Request handlers (write) -----
	FString HandleReplaceFunctionCalls(const FString& Body);
	FString HandleChangeVariableType(const FString& Body);
	FString HandleChangeFunctionParamType(const FString& Body);
	FString HandleRemoveFunctionParameter(const FString& Body);
	FString HandleDeleteAsset(const FString& Body);
	FString HandleDeleteNode(const FString& Body);
	FString HandleDuplicateNodes(const FString& Body);
	FString HandleAddNode(const FString& Body);
	FString HandleRenameAsset(const FString& Body);

	// ----- Validation (read-only, no save) -----
	FString HandleValidateBlueprint(const FString& Body);
	FString HandleValidateAllBlueprints(const FString& Body);

	// ----- Pin manipulation (write) -----
	FString HandleConnectPins(const FString& Body);
	FString HandleDisconnectPin(const FString& Body);
	FString HandleRefreshAllNodes(const FString& Body);
	FString HandleSetPinDefault(const FString& Body);
	FString HandleMoveNode(const FString& Body);
	FString HandleGetNodeComment(const FString& Body);
	FString HandleSetNodeComment(const FString& Body);

	// ----- Pin introspection (read-only) -----
	FString HandleGetPinInfo(const FString& Body);
	FString HandleCheckPinCompatibility(const FString& Body);

	// ----- Class/function discovery (read-only) -----
	FString HandleListClasses(const FString& Body);
	FString HandleListFunctions(const FString& Body);
	FString HandleListProperties(const FString& Body);

	// ----- Struct node manipulation (write) -----
	FString HandleChangeStructNodeType(const FString& Body);

	// ----- Reparent -----
	FString HandleReparentBlueprint(const FString& Body);

	// ----- Create -----
	FString HandleCreateBlueprint(const FString& Body);
	FString HandleCreateGraph(const FString& Body);

	// ----- User-defined types -----
	FString HandleCreateStruct(const FString& Body);
	FString HandleCreateEnum(const FString& Body);
	FString HandleAddStructProperty(const FString& Body);
	FString HandleRemoveStructProperty(const FString& Body);

	// ----- Graph manipulation -----
	FString HandleDeleteGraph(const FString& Body);
	FString HandleRenameGraph(const FString& Body);

	// ----- Variables -----
	FString HandleAddVariable(const FString& Body);
	FString HandleRemoveVariable(const FString& Body);
	FString HandleSetVariableMetadata(const FString& Body);

	// ----- Interfaces -----
	FString HandleAddInterface(const FString& Body);
	FString HandleRemoveInterface(const FString& Body);
	FString HandleListInterfaces(const FString& Body);

	// ----- Event Dispatchers -----
	FString HandleAddEventDispatcher(const FString& Body);
	FString HandleListEventDispatchers(const FString& Body);

	// ----- Function Parameters -----
	FString HandleAddFunctionParameter(const FString& Body);

	// ----- Components -----
	FString HandleAddComponent(const FString& Body);
	FString HandleRemoveComponent(const FString& Body);
	FString HandleListComponents(const FString& Body);

	// ----- Property defaults -----
	FString HandleSetBlueprintDefault(const FString& Body);

	// ----- Diagnostic -----
	FString HandleTestSave(const TMap<FString, FString>& Params);

	// ----- Snapshot / Safety tools (write) -----
	FString HandleSnapshotGraph(const FString& Body);
	FString HandleDiffGraph(const FString& Body);
	FString HandleRestoreGraph(const FString& Body);
	FString HandleFindDisconnectedPins(const FString& Body);
	FString HandleAnalyzeRebuildImpact(const FString& Body);

	// ----- Cross-Blueprint comparison (read-only) -----
	FString HandleDiffBlueprints(const FString& Body);

	// ----- Material read-only handlers (Phase 1) -----
	FString HandleListMaterials(const TMap<FString, FString>& Params);
	FString HandleGetMaterial(const TMap<FString, FString>& Params);
	FString HandleGetMaterialGraph(const TMap<FString, FString>& Params);
	FString HandleDescribeMaterial(const FString& Body);
	FString HandleSearchMaterials(const TMap<FString, FString>& Params);
	FString HandleFindMaterialReferences(const FString& Body);

	// ----- Material mutation handlers (Phase 2) -----
	FString HandleCreateMaterial(const FString& Body);
	FString HandleSetMaterialProperty(const FString& Body);
	FString HandleAddMaterialExpression(const FString& Body);
	FString HandleDeleteMaterialExpression(const FString& Body);
	FString HandleConnectMaterialPins(const FString& Body);
	FString HandleDisconnectMaterialPin(const FString& Body);
	FString HandleSetExpressionValue(const FString& Body);
	FString HandleMoveMaterialExpression(const FString& Body);

	// ----- Material instance handlers (Phase 3) -----
	FString HandleCreateMaterialInstance(const FString& Body);
	FString HandleSetMaterialInstanceParameter(const FString& Body);
	FString HandleGetMaterialInstanceParameters(const TMap<FString, FString>& Params);
	FString HandleReparentMaterialInstance(const FString& Body);

	// ----- Material function handlers (Phase 4) -----
	FString HandleListMaterialFunctions(const TMap<FString, FString>& Params);
	FString HandleGetMaterialFunction(const TMap<FString, FString>& Params);
	FString HandleCreateMaterialFunction(const FString& Body);

	// ----- Material snapshot/diff/restore (Phase 5) -----
	FString HandleSnapshotMaterialGraph(const FString& Body);
	FString HandleDiffMaterialGraph(const FString& Body);
	FString HandleRestoreMaterialGraph(const FString& Body);

	// ----- Serialization -----
	TSharedRef<FJsonObject> SerializeBlueprint(UBlueprint* BP);
	TSharedPtr<FJsonObject> SerializeGraph(UEdGraph* Graph);
	TSharedPtr<FJsonObject> SerializeNode(UEdGraphNode* Node);
	TSharedPtr<FJsonObject> SerializePin(UEdGraphPin* Pin);
	TSharedPtr<FJsonObject> SerializeMaterialExpression(UMaterialExpression* Expression);
	FString JsonToString(TSharedRef<FJsonObject> JsonObj);

	// ----- Helpers -----
	FAssetData* FindBlueprintAsset(const FString& NameOrPath);
	FAssetData* FindMapAsset(const FString& NameOrPath);
	UBlueprint* LoadBlueprintByName(const FString& NameOrPath, FString& OutError);
	UEdGraphNode* FindNodeByGuid(UBlueprint* BP, const FString& GuidString, UEdGraph** OutGraph = nullptr);
	TSharedPtr<FJsonObject> ParseBodyJson(const FString& Body);
	FString MakeErrorJson(const FString& Message);
	bool SaveBlueprintPackage(UBlueprint* BP);
	static FString UrlDecode(const FString& EncodedString);

	// ----- Material helpers -----
	FAssetData* FindMaterialAsset(const FString& NameOrPath);
	UMaterial* LoadMaterialByName(const FString& NameOrPath, FString& OutError);
	FAssetData* FindMaterialInstanceAsset(const FString& NameOrPath);
	UMaterialInstanceConstant* LoadMaterialInstanceByName(const FString& NameOrPath, FString& OutError);
	FAssetData* FindMaterialFunctionAsset(const FString& NameOrPath);
	UMaterialFunction* LoadMaterialFunctionByName(const FString& NameOrPath, FString& OutError);
	bool SaveMaterialPackage(UMaterial* Material);
	bool SaveGenericPackage(UObject* Asset);

	// ----- Type resolution -----
	bool ResolveTypeFromString(const FString& TypeName, FEdGraphPinType& OutPinType, FString& OutError);

	// ----- Snapshot storage -----
	TMap<FString, FGraphSnapshot> Snapshots;
	TMap<FString, FGraphSnapshot> MaterialSnapshots;
	static const int32 MaxSnapshots = 50;

	// Snapshot helpers
	FString GenerateSnapshotId(const FString& BlueprintName);
	FGraphSnapshotData CaptureGraphSnapshot(UEdGraph* Graph);
	void PruneOldSnapshots();
	bool SaveSnapshotToDisk(const FString& SnapshotId, const FGraphSnapshot& Snapshot);
	bool LoadSnapshotFromDisk(const FString& SnapshotId, FGraphSnapshot& OutSnapshot);
};
