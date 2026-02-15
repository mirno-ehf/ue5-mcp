#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Guid.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"

// ============================================================
// Snapshot Helpers
// ============================================================

FString FBlueprintMCPServer::GenerateSnapshotId(const FString& BlueprintName)
{
	FString CleanName = BlueprintName;
	CleanName.ReplaceInline(TEXT("/"), TEXT("_"));
	CleanName.ReplaceInline(TEXT(" "), TEXT("_"));
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	return FString::Printf(TEXT("%s_%s_%s"), *CleanName, *Timestamp, *FGuid::NewGuid().ToString().Left(8));
}

FGraphSnapshotData FBlueprintMCPServer::CaptureGraphSnapshot(UEdGraph* Graph)
{
	FGraphSnapshotData Data;
	if (!Graph) return Data;

	// Record all nodes
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		FNodeRecord Record;
		Record.NodeGuid = Node->NodeGuid.ToString();
		Record.NodeClass = Node->GetClass()->GetName();
		Record.NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		// Check for Break/Make struct type
		if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
		{
			Record.StructType = BreakNode->StructType ? BreakNode->StructType->GetName() : TEXT("<unknown struct>");
		}
		else if (UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
		{
			Record.StructType = MakeNode->StructType ? MakeNode->StructType->GetName() : TEXT("<unknown struct>");
		}

		Data.Nodes.Add(Record);

		// Record ALL pin connections (only from output pins to avoid duplicates)
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction != EGPD_Output) continue;

			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || !Linked->GetOwningNode()) continue;

				FPinConnectionRecord ConnRecord;
				ConnRecord.SourceNodeGuid = Node->NodeGuid.ToString();
				ConnRecord.SourcePinName = Pin->PinName.ToString();
				ConnRecord.TargetNodeGuid = Linked->GetOwningNode()->NodeGuid.ToString();
				ConnRecord.TargetPinName = Linked->PinName.ToString();
				Data.Connections.Add(ConnRecord);
			}
		}
	}

	return Data;
}

void FBlueprintMCPServer::PruneOldSnapshots()
{
	while (Snapshots.Num() > MaxSnapshots)
	{
		FString OldestId;
		FDateTime OldestTime = FDateTime::MaxValue();

		for (const auto& Pair : Snapshots)
		{
			if (Pair.Value.CreatedAt < OldestTime)
			{
				OldestTime = Pair.Value.CreatedAt;
				OldestId = Pair.Key;
			}
		}

		if (!OldestId.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Pruning old snapshot '%s'"), *OldestId);
			Snapshots.Remove(OldestId);
		}
		else
		{
			break;
		}
	}
}

bool FBlueprintMCPServer::SaveSnapshotToDisk(const FString& SnapshotId, const FGraphSnapshot& Snapshot)
{
	FString Dir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("Snapshots");
	IFileManager::Get().MakeDirectory(*Dir, true);

	FString FilePath = Dir / (SnapshotId + TEXT(".json"));

	// Serialize to JSON
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
	Root->SetStringField(TEXT("blueprintName"), Snapshot.BlueprintName);
	Root->SetStringField(TEXT("blueprintPath"), Snapshot.BlueprintPath);
	Root->SetStringField(TEXT("createdAt"), Snapshot.CreatedAt.ToIso8601());

	TSharedRef<FJsonObject> GraphsObj = MakeShared<FJsonObject>();
	for (const auto& GraphPair : Snapshot.Graphs)
	{
		TSharedRef<FJsonObject> GraphObj = MakeShared<FJsonObject>();

		// Nodes
		TArray<TSharedPtr<FJsonValue>> NodesArr;
		for (const FNodeRecord& NodeRec : GraphPair.Value.Nodes)
		{
			TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
			NJ->SetStringField(TEXT("nodeGuid"), NodeRec.NodeGuid);
			NJ->SetStringField(TEXT("nodeClass"), NodeRec.NodeClass);
			NJ->SetStringField(TEXT("nodeTitle"), NodeRec.NodeTitle);
			if (!NodeRec.StructType.IsEmpty())
			{
				NJ->SetStringField(TEXT("structType"), NodeRec.StructType);
			}
			NodesArr.Add(MakeShared<FJsonValueObject>(NJ));
		}
		GraphObj->SetArrayField(TEXT("nodes"), NodesArr);

		// Connections
		TArray<TSharedPtr<FJsonValue>> ConnsArr;
		for (const FPinConnectionRecord& ConnRec : GraphPair.Value.Connections)
		{
			TSharedRef<FJsonObject> CJ = MakeShared<FJsonObject>();
			CJ->SetStringField(TEXT("sourceNodeGuid"), ConnRec.SourceNodeGuid);
			CJ->SetStringField(TEXT("sourcePinName"), ConnRec.SourcePinName);
			CJ->SetStringField(TEXT("targetNodeGuid"), ConnRec.TargetNodeGuid);
			CJ->SetStringField(TEXT("targetPinName"), ConnRec.TargetPinName);
			ConnsArr.Add(MakeShared<FJsonValueObject>(CJ));
		}
		GraphObj->SetArrayField(TEXT("connections"), ConnsArr);

		GraphsObj->SetObjectField(GraphPair.Key, GraphObj);
	}
	Root->SetObjectField(TEXT("graphs"), GraphsObj);

	FString JsonString = JsonToString(Root);
	bool bSuccess = FFileHelper::SaveStringToFile(JsonString, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (bSuccess)
	{
		UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Saved snapshot to disk: %s"), *FilePath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BlueprintMCP: Failed to save snapshot to disk: %s"), *FilePath);
	}
	return bSuccess;
}

bool FBlueprintMCPServer::LoadSnapshotFromDisk(const FString& SnapshotId, FGraphSnapshot& OutSnapshot)
{
	FString Dir = FPaths::ProjectSavedDir() / TEXT("BlueprintMCP") / TEXT("Snapshots");
	FString FilePath = Dir / (SnapshotId + TEXT(".json"));

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	OutSnapshot.SnapshotId = Root->GetStringField(TEXT("snapshotId"));
	OutSnapshot.BlueprintName = Root->GetStringField(TEXT("blueprintName"));
	OutSnapshot.BlueprintPath = Root->GetStringField(TEXT("blueprintPath"));
	FDateTime::ParseIso8601(*Root->GetStringField(TEXT("createdAt")), OutSnapshot.CreatedAt);

	const TSharedPtr<FJsonObject>* GraphsObjPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("graphs"), GraphsObjPtr) && GraphsObjPtr && (*GraphsObjPtr).IsValid())
	{
		for (const auto& GraphPair : (*GraphsObjPtr)->Values)
		{
			FGraphSnapshotData GraphData;
			const TSharedPtr<FJsonObject>& GraphObj = GraphPair.Value->AsObject();
			if (!GraphObj.IsValid()) continue;

			// Nodes
			const TArray<TSharedPtr<FJsonValue>>* NodesArrPtr = nullptr;
			if (GraphObj->TryGetArrayField(TEXT("nodes"), NodesArrPtr))
			{
				for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArrPtr)
				{
					const TSharedPtr<FJsonObject>& NJ = NodeVal->AsObject();
					if (!NJ.IsValid()) continue;

					FNodeRecord NodeRec;
					NodeRec.NodeGuid = NJ->GetStringField(TEXT("nodeGuid"));
					NodeRec.NodeClass = NJ->GetStringField(TEXT("nodeClass"));
					NodeRec.NodeTitle = NJ->GetStringField(TEXT("nodeTitle"));
					NJ->TryGetStringField(TEXT("structType"), NodeRec.StructType);
					GraphData.Nodes.Add(NodeRec);
				}
			}

			// Connections
			const TArray<TSharedPtr<FJsonValue>>* ConnsArrPtr = nullptr;
			if (GraphObj->TryGetArrayField(TEXT("connections"), ConnsArrPtr))
			{
				for (const TSharedPtr<FJsonValue>& ConnVal : *ConnsArrPtr)
				{
					const TSharedPtr<FJsonObject>& CJ = ConnVal->AsObject();
					if (!CJ.IsValid()) continue;

					FPinConnectionRecord ConnRec;
					ConnRec.SourceNodeGuid = CJ->GetStringField(TEXT("sourceNodeGuid"));
					ConnRec.SourcePinName = CJ->GetStringField(TEXT("sourcePinName"));
					ConnRec.TargetNodeGuid = CJ->GetStringField(TEXT("targetNodeGuid"));
					ConnRec.TargetPinName = CJ->GetStringField(TEXT("targetPinName"));
					GraphData.Connections.Add(ConnRec);
				}
			}

			OutSnapshot.Graphs.Add(GraphPair.Key, GraphData);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Loaded snapshot from disk: %s"), *FilePath);
	return true;
}

// ============================================================
// HandleSnapshotGraph
// ============================================================

FString FBlueprintMCPServer::HandleSnapshotGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	if (BlueprintName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: blueprint"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	// Load Blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating snapshot for blueprint '%s'"), *BlueprintName);

	// Build the snapshot
	FGraphSnapshot Snapshot;
	Snapshot.SnapshotId = GenerateSnapshotId(BlueprintName);
	Snapshot.BlueprintName = BP->GetName();
	Snapshot.BlueprintPath = BP->GetPathName();
	Snapshot.CreatedAt = FDateTime::Now();

	// Gather all graphs (UbergraphPages + FunctionGraphs)
	TArray<UEdGraph*> GraphsToCapture;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (!Graph) continue;
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		GraphsToCapture.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (!Graph) continue;
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		GraphsToCapture.Add(Graph);
	}

	if (GraphsToCapture.Num() == 0 && !GraphFilter.IsEmpty())
	{
		return MakeErrorJson(FString::Printf(TEXT("Graph '%s' not found in blueprint '%s'"), *GraphFilter, *BlueprintName));
	}

	int32 TotalConnections = 0;
	TArray<TSharedPtr<FJsonValue>> GraphSummaries;

	for (UEdGraph* Graph : GraphsToCapture)
	{
		FGraphSnapshotData GraphData = CaptureGraphSnapshot(Graph);

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("name"), Graph->GetName());
		Summary->SetNumberField(TEXT("nodeCount"), GraphData.Nodes.Num());
		Summary->SetNumberField(TEXT("connectionCount"), GraphData.Connections.Num());
		GraphSummaries.Add(MakeShared<FJsonValueObject>(Summary));

		TotalConnections += GraphData.Connections.Num();
		Snapshot.Graphs.Add(Graph->GetName(), MoveTemp(GraphData));
	}

	// Store in memory
	Snapshots.Add(Snapshot.SnapshotId, Snapshot);
	PruneOldSnapshots();

	// Save to disk
	SaveSnapshotToDisk(Snapshot.SnapshotId, Snapshot);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Snapshot '%s' created with %d graphs, %d total connections"),
		*Snapshot.SnapshotId, GraphsToCapture.Num(), TotalConnections);

	// Build response
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetArrayField(TEXT("graphs"), GraphSummaries);
	Result->SetNumberField(TEXT("totalConnections"), TotalConnections);
	return JsonToString(Result);
}

// ============================================================
// HandleDiffGraph
// ============================================================

FString FBlueprintMCPServer::HandleDiffGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));
	if (BlueprintName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, snapshotId"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	// Load snapshot from memory or disk
	FGraphSnapshot* SnapshotPtr = Snapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load the current blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Diffing blueprint '%s' against snapshot '%s'"), *BlueprintName, *SnapshotId);

	// Build current state for comparison
	TMap<FString, FGraphSnapshotData> CurrentGraphs;
	TArray<UEdGraph*> AllGraphs;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!GraphFilter.IsEmpty() && Graph->GetName() != GraphFilter) continue;
		CurrentGraphs.Add(Graph->GetName(), CaptureGraphSnapshot(Graph));
	}

	// Build lookup maps for current state
	// Key: "GraphName|SourceGuid|SourcePin|TargetGuid|TargetPin"
	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	// Build node lookup maps: GUID -> NodeRecord
	TMap<FString, const FNodeRecord*> SnapshotNodeMap;
	TMap<FString, const FNodeRecord*> CurrentNodeMap;

	TArray<TSharedPtr<FJsonValue>> SeveredArr;
	TArray<TSharedPtr<FJsonValue>> NewConnsArr;
	TArray<TSharedPtr<FJsonValue>> TypeChangesArr;
	TArray<TSharedPtr<FJsonValue>> MissingNodesArr;

	// Process each graph in the snapshot
	for (const auto& SnapGraphPair : SnapshotPtr->Graphs)
	{
		const FString& GraphName = SnapGraphPair.Key;
		if (!GraphFilter.IsEmpty() && GraphName != GraphFilter) continue;

		const FGraphSnapshotData& SnapData = SnapGraphPair.Value;
		const FGraphSnapshotData* CurDataPtr = CurrentGraphs.Find(GraphName);

		// Build snapshot node map for this graph
		TMap<FString, const FNodeRecord*> SnapNodeLookup;
		for (const FNodeRecord& NR : SnapData.Nodes)
		{
			SnapNodeLookup.Add(NR.NodeGuid, &NR);
		}

		// Build current connection set and node map for this graph
		TSet<FString> CurrentConnSet;
		TMap<FString, const FNodeRecord*> CurNodeLookup;
		if (CurDataPtr)
		{
			for (const FNodeRecord& NR : CurDataPtr->Nodes)
			{
				CurNodeLookup.Add(NR.NodeGuid, &NR);
			}
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
			}
		}

		// Build snapshot connection set
		TSet<FString> SnapConnSet;
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			SnapConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
		}

		// Find severed connections: in snapshot but not in current
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
			if (!CurrentConnSet.Contains(Key))
			{
				TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
				SJ->SetStringField(TEXT("graph"), GraphName);
				SJ->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
				SJ->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
				SJ->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);
				SJ->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);

				// Add node names for readability
				const FNodeRecord** SrcRec = SnapNodeLookup.Find(Conn.SourceNodeGuid);
				if (SrcRec) SJ->SetStringField(TEXT("sourceNodeName"), (*SrcRec)->NodeTitle);
				const FNodeRecord** TgtRec = SnapNodeLookup.Find(Conn.TargetNodeGuid);
				if (TgtRec) SJ->SetStringField(TEXT("targetNodeName"), (*TgtRec)->NodeTitle);

				SeveredArr.Add(MakeShared<FJsonValueObject>(SJ));
			}
		}

		// Find new connections: in current but not in snapshot
		if (CurDataPtr)
		{
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
				if (!SnapConnSet.Contains(Key))
				{
					TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
					NJ->SetStringField(TEXT("graph"), GraphName);
					NJ->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
					NJ->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
					NJ->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);
					NJ->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);

					const FNodeRecord** SrcRec = CurNodeLookup.Find(Conn.SourceNodeGuid);
					if (SrcRec) NJ->SetStringField(TEXT("sourceNodeName"), (*SrcRec)->NodeTitle);
					const FNodeRecord** TgtRec = CurNodeLookup.Find(Conn.TargetNodeGuid);
					if (TgtRec) NJ->SetStringField(TEXT("targetNodeName"), (*TgtRec)->NodeTitle);

					NewConnsArr.Add(MakeShared<FJsonValueObject>(NJ));
				}
			}
		}

		// Find type changes and missing nodes
		for (const FNodeRecord& SnapNode : SnapData.Nodes)
		{
			const FNodeRecord** CurNodePtr = CurNodeLookup.Find(SnapNode.NodeGuid);
			if (!CurNodePtr)
			{
				// Missing node
				TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
				MJ->SetStringField(TEXT("graph"), GraphName);
				MJ->SetStringField(TEXT("nodeGuid"), SnapNode.NodeGuid);
				MJ->SetStringField(TEXT("nodeClass"), SnapNode.NodeClass);
				MJ->SetStringField(TEXT("nodeTitle"), SnapNode.NodeTitle);
				if (!SnapNode.StructType.IsEmpty())
				{
					MJ->SetStringField(TEXT("structType"), SnapNode.StructType);
				}
				MissingNodesArr.Add(MakeShared<FJsonValueObject>(MJ));
			}
			else if (!SnapNode.StructType.IsEmpty())
			{
				// Check for type change on Break/Make nodes
				const FNodeRecord* CurNode = *CurNodePtr;
				if (CurNode->StructType != SnapNode.StructType)
				{
					TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
					TJ->SetStringField(TEXT("graph"), GraphName);
					TJ->SetStringField(TEXT("nodeGuid"), SnapNode.NodeGuid);
					TJ->SetStringField(TEXT("nodeTitle"), SnapNode.NodeTitle);
					TJ->SetStringField(TEXT("oldStructType"), SnapNode.StructType);
					TJ->SetStringField(TEXT("newStructType"), CurNode->StructType);
					TypeChangesArr.Add(MakeShared<FJsonValueObject>(TJ));
				}
			}
		}
	}

	// Build result
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("blueprint"), BP->GetName());
	Result->SetStringField(TEXT("snapshotId"), SnapshotId);
	Result->SetArrayField(TEXT("severedConnections"), SeveredArr);
	Result->SetArrayField(TEXT("newConnections"), NewConnsArr);
	Result->SetArrayField(TEXT("typeChanges"), TypeChangesArr);
	Result->SetArrayField(TEXT("missingNodes"), MissingNodesArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("severedConnections"), SeveredArr.Num());
	Summary->SetNumberField(TEXT("newConnections"), NewConnsArr.Num());
	Summary->SetNumberField(TEXT("typeChanges"), TypeChangesArr.Num());
	Summary->SetNumberField(TEXT("missingNodes"), MissingNodesArr.Num());
	Result->SetObjectField(TEXT("summary"), Summary);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Diff complete — %d severed, %d new, %d type changes, %d missing nodes"),
		SeveredArr.Num(), NewConnsArr.Num(), TypeChangesArr.Num(), MissingNodesArr.Num());

	return JsonToString(Result);
}

// ============================================================
// HandleRestoreGraph
// ============================================================

FString FBlueprintMCPServer::HandleRestoreGraph(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString SnapshotId = Json->GetStringField(TEXT("snapshotId"));
	if (BlueprintName.IsEmpty() || SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, snapshotId"));
	}

	FString GraphFilter;
	Json->TryGetStringField(TEXT("graph"), GraphFilter);

	FString NodeIdFilter;
	Json->TryGetStringField(TEXT("nodeId"), NodeIdFilter);

	bool bDryRun = false;
	Json->TryGetBoolField(TEXT("dryRun"), bDryRun);

	// Load snapshot from memory or disk
	FGraphSnapshot* SnapshotPtr = Snapshots.Find(SnapshotId);
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotPtr)
	{
		if (!LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
		{
			return MakeErrorJson(FString::Printf(TEXT("Snapshot '%s' not found in memory or on disk"), *SnapshotId));
		}
		SnapshotPtr = &LoadedSnapshot;
	}

	// Load the current blueprint
	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Restoring connections from snapshot '%s' for blueprint '%s' (dryRun=%s)"),
		*SnapshotId, *BlueprintName, bDryRun ? TEXT("true") : TEXT("false"));

	// Build current connection set for comparison
	TMap<FString, FGraphSnapshotData> CurrentGraphs;
	TArray<UEdGraph*> AllGraphs;
	for (UEdGraph* Graph : BP->UbergraphPages)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph) AllGraphs.Add(Graph);
	}
	for (UEdGraph* Graph : AllGraphs)
	{
		CurrentGraphs.Add(Graph->GetName(), CaptureGraphSnapshot(Graph));
	}

	auto MakeConnKey = [](const FString& SrcGuid, const FString& SrcPin, const FString& TgtGuid, const FString& TgtPin) -> FString
	{
		return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcGuid, *SrcPin, *TgtGuid, *TgtPin);
	};

	int32 Reconnected = 0;
	int32 Failed = 0;
	TArray<TSharedPtr<FJsonValue>> DetailsArr;

	for (const auto& SnapGraphPair : SnapshotPtr->Graphs)
	{
		const FString& GraphName = SnapGraphPair.Key;
		if (!GraphFilter.IsEmpty() && GraphName != GraphFilter) continue;

		const FGraphSnapshotData& SnapData = SnapGraphPair.Value;
		const FGraphSnapshotData* CurDataPtr = CurrentGraphs.Find(GraphName);

		// Build current connection set
		TSet<FString> CurrentConnSet;
		if (CurDataPtr)
		{
			for (const FPinConnectionRecord& Conn : CurDataPtr->Connections)
			{
				CurrentConnSet.Add(MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName));
			}
		}

		// Find severed connections and try to restore them
		for (const FPinConnectionRecord& Conn : SnapData.Connections)
		{
			FString Key = MakeConnKey(Conn.SourceNodeGuid, Conn.SourcePinName, Conn.TargetNodeGuid, Conn.TargetPinName);
			if (CurrentConnSet.Contains(Key)) continue; // Still connected, skip

			// Apply nodeId filter if specified
			if (!NodeIdFilter.IsEmpty() && Conn.SourceNodeGuid != NodeIdFilter && Conn.TargetNodeGuid != NodeIdFilter)
			{
				continue;
			}

			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("graph"), GraphName);
			Detail->SetStringField(TEXT("sourcePinName"), Conn.SourcePinName);
			Detail->SetStringField(TEXT("targetPinName"), Conn.TargetPinName);
			Detail->SetStringField(TEXT("sourceNodeGuid"), Conn.SourceNodeGuid);
			Detail->SetStringField(TEXT("targetNodeGuid"), Conn.TargetNodeGuid);

			// Find source and target nodes
			UEdGraph* SourceGraph = nullptr;
			UEdGraphNode* SourceNode = FindNodeByGuid(BP, Conn.SourceNodeGuid, &SourceGraph);
			UEdGraphNode* TargetNode = FindNodeByGuid(BP, Conn.TargetNodeGuid);

			if (!SourceNode)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Source node '%s' no longer exists"), *Conn.SourceNodeGuid));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}
			if (!TargetNode)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Target node '%s' no longer exists"), *Conn.TargetNodeGuid));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			Detail->SetStringField(TEXT("sourceNodeName"), SourceNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			Detail->SetStringField(TEXT("targetNodeName"), TargetNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

			// Find pins
			UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*Conn.SourcePinName));
			UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*Conn.TargetPinName));

			if (!SourcePin)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Source pin '%s' not found on node"), *Conn.SourcePinName));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}
			if (!TargetPin)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(TEXT("Target pin '%s' not found on node"), *Conn.TargetPinName));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			if (bDryRun)
			{
				// In dry run, just report what would happen
				Detail->SetStringField(TEXT("result"), TEXT("would_reconnect"));
				Reconnected++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			// Try type-validated connection via the schema
			const UEdGraphSchema* Schema = SourceGraph ? SourceGraph->GetSchema() : nullptr;
			if (!Schema)
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), TEXT("Graph schema not found"));
				Failed++;
				DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
				continue;
			}

			bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);
			if (bConnected)
			{
				Detail->SetStringField(TEXT("result"), TEXT("reconnected"));
				Reconnected++;
			}
			else
			{
				Detail->SetStringField(TEXT("result"), TEXT("failed"));
				Detail->SetStringField(TEXT("reason"), FString::Printf(
					TEXT("TryCreateConnection failed — types may be incompatible (%s -> %s)"),
					*SourcePin->PinType.PinCategory.ToString(),
					*TargetPin->PinType.PinCategory.ToString()));
				Failed++;
			}
			DetailsArr.Add(MakeShared<FJsonValueObject>(Detail));
		}
	}

	// Save if not dry run and we reconnected something
	bool bSaved = false;
	if (!bDryRun && Reconnected > 0)
	{
		bSaved = SaveBlueprintPackage(BP);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Restore complete — %d reconnected, %d failed, saved=%s"),
		Reconnected, Failed, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetNumberField(TEXT("reconnected"), Reconnected);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetArrayField(TEXT("details"), DetailsArr);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	return JsonToString(Result);
}

// ============================================================
// HandleFindDisconnectedPins
// ============================================================

FString FBlueprintMCPServer::HandleFindDisconnectedPins(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName;
	Json->TryGetStringField(TEXT("blueprint"), BlueprintName);

	FString PathFilter;
	Json->TryGetStringField(TEXT("filter"), PathFilter);

	FString SnapshotId;
	Json->TryGetStringField(TEXT("snapshotId"), SnapshotId);

	if (BlueprintName.IsEmpty() && PathFilter.IsEmpty() && SnapshotId.IsEmpty())
	{
		return MakeErrorJson(TEXT("Provide at least one of: blueprint, filter, or snapshotId"));
	}

	// Optionally load snapshot for definite-break detection
	FGraphSnapshot* SnapshotPtr = nullptr;
	FGraphSnapshot LoadedSnapshot;
	if (!SnapshotId.IsEmpty())
	{
		SnapshotPtr = Snapshots.Find(SnapshotId);
		if (!SnapshotPtr)
		{
			if (LoadSnapshotFromDisk(SnapshotId, LoadedSnapshot))
			{
				SnapshotPtr = &LoadedSnapshot;
			}
		}
	}

	// Build snapshot connection lookup: "nodeGuid|pinName" -> array of connected targets
	TMap<FString, TArray<FPinConnectionRecord>> SnapConnByNode;
	if (SnapshotPtr)
	{
		for (const auto& GraphPair : SnapshotPtr->Graphs)
		{
			for (const FPinConnectionRecord& Conn : GraphPair.Value.Connections)
			{
				FString SrcKey = FString::Printf(TEXT("%s|%s"), *Conn.SourceNodeGuid, *Conn.SourcePinName);
				SnapConnByNode.FindOrAdd(SrcKey).Add(Conn);
				FString TgtKey = FString::Printf(TEXT("%s|%s"), *Conn.TargetNodeGuid, *Conn.TargetPinName);
				SnapConnByNode.FindOrAdd(TgtKey).Add(Conn);
			}
		}
	}

	// Collect blueprints to scan
	TArray<FString> BlueprintsToScan;
	if (!BlueprintName.IsEmpty())
	{
		BlueprintsToScan.Add(BlueprintName);
	}
	else if (!PathFilter.IsEmpty())
	{
		for (const FAssetData& Asset : AllBlueprintAssets)
		{
			if (Asset.PackageName.ToString().Contains(PathFilter) || Asset.AssetName.ToString().Contains(PathFilter))
			{
				BlueprintsToScan.Add(Asset.AssetName.ToString());
			}
		}
	}
	else if (SnapshotPtr)
	{
		// Use the snapshot's blueprint
		BlueprintsToScan.Add(SnapshotPtr->BlueprintName);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Finding disconnected pins across %d blueprint(s)"), BlueprintsToScan.Num());

	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	int32 HighCount = 0;
	int32 MediumCount = 0;
	int32 BlueprintsScanned = 0;

	for (const FString& BPName : BlueprintsToScan)
	{
		FString LoadError;
		UBlueprint* BP = LoadBlueprintByName(BPName, LoadError);
		if (!BP) continue;
		BlueprintsScanned++;

		TArray<UEdGraph*> AllGraphs;
		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph) AllGraphs.Add(Graph);
		}
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph) AllGraphs.Add(Graph);
		}

		for (UEdGraph* Graph : AllGraphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node);
				UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node);

				// Only inspect Break/Make struct nodes for heuristic detection
				bool bIsBreakMakeNode = (BreakNode || MakeNode);

				FString StructTypeName;
				if (BreakNode)
				{
					StructTypeName = BreakNode->StructType ? BreakNode->StructType->GetName() : TEXT("<unknown struct>");
				}
				else if (MakeNode)
				{
					StructTypeName = MakeNode->StructType ? MakeNode->StructType->GetName() : TEXT("<unknown struct>");
				}

				// Heuristic detection for Break/Make nodes
				if (bIsBreakMakeNode)
				{
					bool bUnresolvedStruct = StructTypeName.Contains(TEXT("unknown")) ||
						StructTypeName.Equals(TEXT("None")) || StructTypeName.IsEmpty();

					if (bUnresolvedStruct)
					{
						// HIGH confidence: unresolved struct
						TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("blueprint"), BP->GetName());
						Item->SetStringField(TEXT("graph"), Graph->GetName());
						Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
						Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						Item->SetStringField(TEXT("structType"), StructTypeName);
						Item->SetStringField(TEXT("confidence"), TEXT("HIGH"));
						Item->SetStringField(TEXT("reason"), TEXT("Unresolved or unknown struct type"));

						// List pins
						TArray<TSharedPtr<FJsonValue>> PinsArr;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
							PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
							PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

							// Check snapshot for what it was connected to
							FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
							if (SnapConnByNode.Contains(PinKey))
							{
								TArray<TSharedPtr<FJsonValue>> WasConnArr;
								for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
								{
									FString OtherGuid = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetNodeGuid : CR.SourceNodeGuid;
									FString OtherPin = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetPinName : CR.SourcePinName;
									WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPin)));
								}
								PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
							}
							PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
						}
						Item->SetArrayField(TEXT("pins"), PinsArr);
						ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
						HighCount++;
					}
					else
					{
						// Check for MEDIUM: valid struct but zero data pin connections
						bool bHasDataConnection = false;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							if (Pin->LinkedTo.Num() > 0)
							{
								bHasDataConnection = true;
								break;
							}
						}

						if (!bHasDataConnection)
						{
							TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("blueprint"), BP->GetName());
							Item->SetStringField(TEXT("graph"), Graph->GetName());
							Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
							Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
							Item->SetStringField(TEXT("structType"), StructTypeName);
							Item->SetStringField(TEXT("confidence"), TEXT("MEDIUM"));
							Item->SetStringField(TEXT("reason"), TEXT("Break/Make node with valid struct but zero data pin connections"));

							TArray<TSharedPtr<FJsonValue>> PinsArr;
							for (UEdGraphPin* Pin : Node->Pins)
							{
								if (!Pin || Pin->bHidden) continue;
								if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
								TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
								PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
								PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

								FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
								if (SnapConnByNode.Contains(PinKey))
								{
									TArray<TSharedPtr<FJsonValue>> WasConnArr;
									for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
									{
										FString OtherGuid = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetNodeGuid : CR.SourceNodeGuid;
										FString OtherPin = (CR.SourceNodeGuid == Node->NodeGuid.ToString()) ? CR.TargetPinName : CR.SourcePinName;
										WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPin)));
									}
									PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
								}
								PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
							}
							Item->SetArrayField(TEXT("pins"), PinsArr);
							ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
							MediumCount++;
						}
					}
				}

				// Snapshot-based definite-break detection (applies to ALL node types)
				if (SnapshotPtr)
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->bHidden) continue;
						FString PinKey = FString::Printf(TEXT("%s|%s"), *Node->NodeGuid.ToString(), *Pin->PinName.ToString());
						if (!SnapConnByNode.Contains(PinKey)) continue;

						// This pin had connections in the snapshot — check if any are now missing
						for (const FPinConnectionRecord& CR : SnapConnByNode[PinKey])
						{
							// Determine which side is "other"
							bool bWeAreSource = (CR.SourceNodeGuid == Node->NodeGuid.ToString() && CR.SourcePinName == Pin->PinName.ToString());
							FString OtherGuid = bWeAreSource ? CR.TargetNodeGuid : CR.SourceNodeGuid;
							FString OtherPinName = bWeAreSource ? CR.TargetPinName : CR.SourcePinName;

							// Check if this connection still exists
							bool bStillConnected = false;
							for (UEdGraphPin* Linked : Pin->LinkedTo)
							{
								if (Linked && Linked->GetOwningNode() &&
									Linked->GetOwningNode()->NodeGuid.ToString() == OtherGuid &&
									Linked->PinName.ToString() == OtherPinName)
								{
									bStillConnected = true;
									break;
								}
							}

							if (!bStillConnected)
							{
								// Skip if we already reported this node above via heuristic
								// Only add for non-Break/Make nodes, or if the Break/Make node wasn't caught by heuristics
								if (!bIsBreakMakeNode)
								{
									TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
									Item->SetStringField(TEXT("blueprint"), BP->GetName());
									Item->SetStringField(TEXT("graph"), Graph->GetName());
									Item->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
									Item->SetStringField(TEXT("nodeTitle"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
									if (!StructTypeName.IsEmpty())
									{
										Item->SetStringField(TEXT("structType"), StructTypeName);
									}
									Item->SetStringField(TEXT("confidence"), TEXT("HIGH"));
									Item->SetStringField(TEXT("reason"), TEXT("Connection existed in snapshot but is now missing"));

									TArray<TSharedPtr<FJsonValue>> PinsArr;
									TSharedRef<FJsonObject> PinJ = MakeShared<FJsonObject>();
									PinJ->SetStringField(TEXT("name"), Pin->PinName.ToString());
									PinJ->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
									TArray<TSharedPtr<FJsonValue>> WasConnArr;
									WasConnArr.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s"), *OtherGuid, *OtherPinName)));
									PinJ->SetArrayField(TEXT("wasConnectedTo"), WasConnArr);
									PinsArr.Add(MakeShared<FJsonValueObject>(PinJ));
									Item->SetArrayField(TEXT("pins"), PinsArr);

									ResultsArr.Add(MakeShared<FJsonValueObject>(Item));
									HighCount++;
								}
								break; // Only report once per node per snapshot-check pass
							}
						}
					}
				}
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: FindDisconnectedPins complete — %d HIGH, %d MEDIUM, %d total across %d blueprints"),
		HighCount, MediumCount, ResultsArr.Num(), BlueprintsScanned);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("results"), ResultsArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("high"), HighCount);
	Summary->SetNumberField(TEXT("medium"), MediumCount);
	Summary->SetNumberField(TEXT("total"), ResultsArr.Num());
	Summary->SetNumberField(TEXT("blueprintsScanned"), BlueprintsScanned);
	Result->SetObjectField(TEXT("summary"), Summary);

	return JsonToString(Result);
}

// ============================================================
// HandleAnalyzeRebuildImpact
// ============================================================

FString FBlueprintMCPServer::HandleAnalyzeRebuildImpact(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString ModuleName = Json->GetStringField(TEXT("moduleName"));
	if (ModuleName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: moduleName"));
	}

	// Optional struct name filter
	TArray<FString> StructNameFilter;
	const TArray<TSharedPtr<FJsonValue>>* StructNamesArr = nullptr;
	if (Json->TryGetArrayField(TEXT("structNames"), StructNamesArr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *StructNamesArr)
		{
			FString Name = Val->AsString();
			if (!Name.IsEmpty())
			{
				StructNameFilter.Add(Name);
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Analyzing rebuild impact for module '%s'"), *ModuleName);

	// Enumerate all UScriptStruct and UEnum objects belonging to this module
	TArray<UScriptStruct*> FoundStructs;
	TArray<UEnum*> FoundEnums;

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		if (!Struct) continue;

		FString PackageName = Struct->GetOutermost()->GetName();
		if (!PackageName.Contains(ModuleName)) continue;

		// Apply name filter if provided
		if (StructNameFilter.Num() > 0)
		{
			bool bMatch = false;
			for (const FString& FilterName : StructNameFilter)
			{
				// Match with or without F prefix
				FString CleanFilter = FilterName;
				if (CleanFilter.StartsWith(TEXT("F")))
				{
					CleanFilter = CleanFilter.Mid(1);
				}
				if (Struct->GetName() == FilterName || Struct->GetName() == CleanFilter)
				{
					bMatch = true;
					break;
				}
			}
			if (!bMatch) continue;
		}

		FoundStructs.Add(Struct);
	}

	for (TObjectIterator<UEnum> It; It; ++It)
	{
		UEnum* Enum = *It;
		if (!Enum) continue;

		FString PackageName = Enum->GetOutermost()->GetName();
		if (!PackageName.Contains(ModuleName)) continue;

		if (StructNameFilter.Num() > 0)
		{
			bool bMatch = false;
			for (const FString& FilterName : StructNameFilter)
			{
				FString CleanFilter = FilterName;
				if (CleanFilter.StartsWith(TEXT("E")))
				{
					CleanFilter = CleanFilter.Mid(1);
				}
				if (Enum->GetName() == FilterName || Enum->GetName() == CleanFilter)
				{
					bMatch = true;
					break;
				}
			}
			if (!bMatch) continue;
		}

		FoundEnums.Add(Enum);
	}

	// Build list of found types
	TArray<TSharedPtr<FJsonValue>> TypesFoundArr;
	for (UScriptStruct* S : FoundStructs)
	{
		TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
		TJ->SetStringField(TEXT("name"), S->GetName());
		TJ->SetStringField(TEXT("kind"), TEXT("struct"));
		TJ->SetStringField(TEXT("package"), S->GetOutermost()->GetName());
		TypesFoundArr.Add(MakeShared<FJsonValueObject>(TJ));
	}
	for (UEnum* E : FoundEnums)
	{
		TSharedRef<FJsonObject> TJ = MakeShared<FJsonObject>();
		TJ->SetStringField(TEXT("name"), E->GetName());
		TJ->SetStringField(TEXT("kind"), TEXT("enum"));
		TJ->SetStringField(TEXT("package"), E->GetOutermost()->GetName());
		TypesFoundArr.Add(MakeShared<FJsonValueObject>(TJ));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Found %d structs and %d enums in module '%s'"),
		FoundStructs.Num(), FoundEnums.Num(), *ModuleName);

	// Build a set of type names for fast lookup
	TSet<FString> TypeNameSet;
	for (UScriptStruct* S : FoundStructs) TypeNameSet.Add(S->GetName());
	for (UEnum* E : FoundEnums) TypeNameSet.Add(E->GetName());

	// Scan all blueprints for references
	struct FBlueprintImpact
	{
		FString Name;
		FString Path;
		int32 BreakNodes = 0;
		int32 MakeNodes = 0;
		int32 Variables = 0;
		int32 FunctionParams = 0;
		int32 ConnectionsAtRisk = 0;
		FString Risk;
	};

	TArray<FBlueprintImpact> AffectedBlueprints;
	int32 TotalBreakMakeNodes = 0;
	int32 TotalConnectionsAtRisk = 0;

	for (const FAssetData& Asset : AllBlueprintAssets)
	{
		FString LoadError;
		UBlueprint* BP = LoadBlueprintByName(Asset.AssetName.ToString(), LoadError);
		if (!BP) continue;

		FBlueprintImpact Impact;
		Impact.Name = BP->GetName();
		Impact.Path = BP->GetPathName();

		// Scan graphs for Break/Make struct nodes
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);

		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
				{
					if (BreakNode->StructType && TypeNameSet.Contains(BreakNode->StructType->GetName()))
					{
						Impact.BreakNodes++;
						// Count data connections at risk
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							Impact.ConnectionsAtRisk += Pin->LinkedTo.Num();
						}
					}
				}
				else if (UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node))
				{
					if (MakeNode->StructType && TypeNameSet.Contains(MakeNode->StructType->GetName()))
					{
						Impact.MakeNodes++;
						for (UEdGraphPin* Pin : Node->Pins)
						{
							if (!Pin || Pin->bHidden) continue;
							if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
							Impact.ConnectionsAtRisk += Pin->LinkedTo.Num();
						}
					}
				}
			}
		}

		// Scan variables
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarType.PinSubCategoryObject.IsValid())
			{
				FString SubObjName = Var.VarType.PinSubCategoryObject->GetName();
				if (TypeNameSet.Contains(SubObjName))
				{
					Impact.Variables++;
				}
			}
		}

		// Scan function parameters
		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node);
				if (!FuncEntry) continue;

				for (const TSharedPtr<FUserPinInfo>& PinInfo : FuncEntry->UserDefinedPins)
				{
					if (!PinInfo.IsValid()) continue;
					if (PinInfo->PinType.PinSubCategoryObject.IsValid())
					{
						FString SubObjName = PinInfo->PinType.PinSubCategoryObject->GetName();
						if (TypeNameSet.Contains(SubObjName))
						{
							Impact.FunctionParams++;
						}
					}
				}
			}
		}

		// Only include if this BP is affected
		if (Impact.BreakNodes > 0 || Impact.MakeNodes > 0 || Impact.Variables > 0 || Impact.FunctionParams > 0)
		{
			// Classify risk
			if (Impact.BreakNodes > 0 || Impact.MakeNodes > 0)
			{
				Impact.Risk = TEXT("HIGH");
			}
			else if (Impact.Variables > 0)
			{
				Impact.Risk = TEXT("MEDIUM");
			}
			else
			{
				Impact.Risk = TEXT("LOW");
			}

			TotalBreakMakeNodes += Impact.BreakNodes + Impact.MakeNodes;
			TotalConnectionsAtRisk += Impact.ConnectionsAtRisk;
			AffectedBlueprints.Add(Impact);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Rebuild impact analysis complete — %d affected blueprints, %d Break/Make nodes, %d connections at risk"),
		AffectedBlueprints.Num(), TotalBreakMakeNodes, TotalConnectionsAtRisk);

	// Build response
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("moduleName"), ModuleName);
	Result->SetArrayField(TEXT("typesFound"), TypesFoundArr);

	TArray<TSharedPtr<FJsonValue>> AffectedArr;
	for (const FBlueprintImpact& Impact : AffectedBlueprints)
	{
		TSharedRef<FJsonObject> BJ = MakeShared<FJsonObject>();
		BJ->SetStringField(TEXT("name"), Impact.Name);
		BJ->SetStringField(TEXT("path"), Impact.Path);
		BJ->SetNumberField(TEXT("breakNodes"), Impact.BreakNodes);
		BJ->SetNumberField(TEXT("makeNodes"), Impact.MakeNodes);
		BJ->SetNumberField(TEXT("variables"), Impact.Variables);
		BJ->SetNumberField(TEXT("functionParams"), Impact.FunctionParams);
		BJ->SetNumberField(TEXT("connectionsAtRisk"), Impact.ConnectionsAtRisk);
		BJ->SetStringField(TEXT("risk"), Impact.Risk);
		AffectedArr.Add(MakeShared<FJsonValueObject>(BJ));
	}
	Result->SetArrayField(TEXT("affectedBlueprints"), AffectedArr);

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("totalBlueprints"), AffectedBlueprints.Num());
	Summary->SetNumberField(TEXT("totalBreakMakeNodes"), TotalBreakMakeNodes);
	Summary->SetNumberField(TEXT("totalConnectionsAtRisk"), TotalConnectionsAtRisk);
	Result->SetObjectField(TEXT("summary"), Summary);

	return JsonToString(Result);
}
