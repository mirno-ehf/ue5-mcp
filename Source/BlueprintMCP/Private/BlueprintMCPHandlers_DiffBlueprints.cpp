#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// HandleDiffBlueprints — structural diff between two Blueprints
// ============================================================

FString FBlueprintMCPServer::HandleDiffBlueprints(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintA = Json->GetStringField(TEXT("blueprintA"));
	FString BlueprintB = Json->GetStringField(TEXT("blueprintB"));
	FString GraphFilter = Json->GetStringField(TEXT("graph"));

	if (BlueprintA.IsEmpty() || BlueprintB.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprintA, blueprintB"));
	}

	// Load both blueprints
	FString LoadErrorA, LoadErrorB;
	UBlueprint* BPA = LoadBlueprintByName(BlueprintA, LoadErrorA);
	if (!BPA) return MakeErrorJson(FString::Printf(TEXT("blueprintA: %s"), *LoadErrorA));

	UBlueprint* BPB = LoadBlueprintByName(BlueprintB, LoadErrorB);
	if (!BPB) return MakeErrorJson(FString::Printf(TEXT("blueprintB: %s"), *LoadErrorB));

	// Helper to gather graphs from a Blueprint
	auto GatherGraphs = [&GraphFilter](UBlueprint* BP) -> TArray<UEdGraph*>
	{
		TArray<UEdGraph*> Graphs;
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (!G) continue;
			if (!GraphFilter.IsEmpty() && G->GetName() != GraphFilter) continue;
			Graphs.Add(G);
		}
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (!G) continue;
			if (!GraphFilter.IsEmpty() && G->GetName() != GraphFilter) continue;
			Graphs.Add(G);
		}
		return Graphs;
	};

	TArray<UEdGraph*> GraphsA = GatherGraphs(BPA);
	TArray<UEdGraph*> GraphsB = GatherGraphs(BPB);

	// Build graph name maps
	TMap<FString, UEdGraph*> GraphMapA, GraphMapB;
	for (UEdGraph* G : GraphsA) GraphMapA.Add(G->GetName(), G);
	for (UEdGraph* G : GraphsB) GraphMapB.Add(G->GetName(), G);

	// Compare graphs
	TArray<TSharedPtr<FJsonValue>> GraphDiffs;

	// Find all unique graph names
	TSet<FString> AllGraphNames;
	for (auto& Pair : GraphMapA) AllGraphNames.Add(Pair.Key);
	for (auto& Pair : GraphMapB) AllGraphNames.Add(Pair.Key);

	for (const FString& GraphName : AllGraphNames)
	{
		UEdGraph** pGA = GraphMapA.Find(GraphName);
		UEdGraph** pGB = GraphMapB.Find(GraphName);

		TSharedRef<FJsonObject> GD = MakeShared<FJsonObject>();
		GD->SetStringField(TEXT("graph"), GraphName);

		if (!pGA)
		{
			GD->SetStringField(TEXT("status"), TEXT("onlyInB"));
			GD->SetNumberField(TEXT("nodeCountB"), (*pGB)->Nodes.Num());
			GraphDiffs.Add(MakeShared<FJsonValueObject>(GD));
			continue;
		}
		if (!pGB)
		{
			GD->SetStringField(TEXT("status"), TEXT("onlyInA"));
			GD->SetNumberField(TEXT("nodeCountA"), (*pGA)->Nodes.Num());
			GraphDiffs.Add(MakeShared<FJsonValueObject>(GD));
			continue;
		}

		// Both exist — compare nodes
		UEdGraph* GA = *pGA;
		UEdGraph* GB = *pGB;

		// Build node title maps for matching (title -> node list for each)
		TMap<FString, TArray<UEdGraphNode*>> NodesA, NodesB;
		for (UEdGraphNode* N : GA->Nodes)
		{
			if (!N) continue;
			FString Title = N->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			NodesA.FindOrAdd(Title).Add(N);
		}
		for (UEdGraphNode* N : GB->Nodes)
		{
			if (!N) continue;
			FString Title = N->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			NodesB.FindOrAdd(Title).Add(N);
		}

		// Nodes only in A
		TArray<TSharedPtr<FJsonValue>> OnlyInA;
		for (auto& Pair : NodesA)
		{
			int32 CountA = Pair.Value.Num();
			int32 CountB = 0;
			if (TArray<UEdGraphNode*>* pArr = NodesB.Find(Pair.Key))
			{
				CountB = pArr->Num();
			}
			if (CountA > CountB)
			{
				TSharedRef<FJsonObject> NObj = MakeShared<FJsonObject>();
				NObj->SetStringField(TEXT("title"), Pair.Key);
				NObj->SetStringField(TEXT("class"), Pair.Value[0]->GetClass()->GetName());
				NObj->SetNumberField(TEXT("extraCount"), CountA - CountB);
				OnlyInA.Add(MakeShared<FJsonValueObject>(NObj));
			}
		}

		// Nodes only in B
		TArray<TSharedPtr<FJsonValue>> OnlyInB;
		for (auto& Pair : NodesB)
		{
			int32 CountB = Pair.Value.Num();
			int32 CountA = 0;
			if (TArray<UEdGraphNode*>* pArr = NodesA.Find(Pair.Key))
			{
				CountA = pArr->Num();
			}
			if (CountB > CountA)
			{
				TSharedRef<FJsonObject> NObj = MakeShared<FJsonObject>();
				NObj->SetStringField(TEXT("title"), Pair.Key);
				NObj->SetStringField(TEXT("class"), Pair.Value[0]->GetClass()->GetName());
				NObj->SetNumberField(TEXT("extraCount"), CountB - CountA);
				OnlyInB.Add(MakeShared<FJsonValueObject>(NObj));
			}
		}

		// Connection diff: use connection key approach
		auto MakeConnKey = [](UEdGraphPin* SrcPin, UEdGraphPin* TgtPin) -> FString
		{
			FString SrcTitle = SrcPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString TgtTitle = TgtPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			return FString::Printf(TEXT("%s|%s|%s|%s"), *SrcTitle, *SrcPin->PinName.ToString(), *TgtTitle, *TgtPin->PinName.ToString());
		};

		TSet<FString> ConnectionsA, ConnectionsB;
		for (UEdGraphNode* N : GA->Nodes)
		{
			if (!N) continue;
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					ConnectionsA.Add(MakeConnKey(Pin, Linked));
				}
			}
		}
		for (UEdGraphNode* N : GB->Nodes)
		{
			if (!N) continue;
			for (UEdGraphPin* Pin : N->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->GetOwningNode()) continue;
					ConnectionsB.Add(MakeConnKey(Pin, Linked));
				}
			}
		}

		TArray<TSharedPtr<FJsonValue>> ConnsOnlyInA, ConnsOnlyInB;
		for (const FString& Key : ConnectionsA)
		{
			if (!ConnectionsB.Contains(Key))
			{
				ConnsOnlyInA.Add(MakeShared<FJsonValueString>(Key));
			}
		}
		for (const FString& Key : ConnectionsB)
		{
			if (!ConnectionsA.Contains(Key))
			{
				ConnsOnlyInB.Add(MakeShared<FJsonValueString>(Key));
			}
		}

		bool bIdentical = OnlyInA.Num() == 0 && OnlyInB.Num() == 0 && ConnsOnlyInA.Num() == 0 && ConnsOnlyInB.Num() == 0;
		GD->SetStringField(TEXT("status"), bIdentical ? TEXT("identical") : TEXT("different"));
		GD->SetNumberField(TEXT("nodeCountA"), GA->Nodes.Num());
		GD->SetNumberField(TEXT("nodeCountB"), GB->Nodes.Num());

		if (OnlyInA.Num() > 0) GD->SetArrayField(TEXT("nodesOnlyInA"), OnlyInA);
		if (OnlyInB.Num() > 0) GD->SetArrayField(TEXT("nodesOnlyInB"), OnlyInB);
		if (ConnsOnlyInA.Num() > 0) GD->SetArrayField(TEXT("connectionsOnlyInA"), ConnsOnlyInA);
		if (ConnsOnlyInB.Num() > 0) GD->SetArrayField(TEXT("connectionsOnlyInB"), ConnsOnlyInB);

		GraphDiffs.Add(MakeShared<FJsonValueObject>(GD));
	}

	// Compare variables
	TArray<TSharedPtr<FJsonValue>> VarsOnlyInA, VarsOnlyInB;
	TSet<FString> VarNamesA, VarNamesB;
	for (const FBPVariableDescription& V : BPA->NewVariables) VarNamesA.Add(V.VarName.ToString());
	for (const FBPVariableDescription& V : BPB->NewVariables) VarNamesB.Add(V.VarName.ToString());

	for (const FString& Name : VarNamesA)
	{
		if (!VarNamesB.Contains(Name))
		{
			VarsOnlyInA.Add(MakeShared<FJsonValueString>(Name));
		}
	}
	for (const FString& Name : VarNamesB)
	{
		if (!VarNamesA.Contains(Name))
		{
			VarsOnlyInB.Add(MakeShared<FJsonValueString>(Name));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprintA"), BlueprintA);
	Result->SetStringField(TEXT("blueprintB"), BlueprintB);
	Result->SetArrayField(TEXT("graphs"), GraphDiffs);

	if (VarsOnlyInA.Num() > 0) Result->SetArrayField(TEXT("variablesOnlyInA"), VarsOnlyInA);
	if (VarsOnlyInB.Num() > 0) Result->SetArrayField(TEXT("variablesOnlyInB"), VarsOnlyInB);

	// Summary counts
	int32 TotalDiffs = 0;
	for (auto& GDVal : GraphDiffs)
	{
		auto GDObj = GDVal->AsObject();
		FString Status = GDObj->GetStringField(TEXT("status"));
		if (Status != TEXT("identical")) TotalDiffs++;
	}
	TotalDiffs += VarsOnlyInA.Num() + VarsOnlyInB.Num();
	Result->SetNumberField(TEXT("totalDifferences"), TotalDiffs);

	return JsonToString(Result);
}
