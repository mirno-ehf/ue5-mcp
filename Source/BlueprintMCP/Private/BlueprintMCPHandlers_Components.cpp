#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"

// ============================================================
// HandleListComponents — list all components in a Blueprint's SCS
// ============================================================

FString FBlueprintMCPServer::HandleListComponents(const FString& Body)
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

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Blueprint '%s' does not have a SimpleConstructionScript (not an Actor Blueprint)"),
			*BlueprintName));
	}

	const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();

	TArray<TSharedPtr<FJsonValue>> ComponentsArr;
	for (USCS_Node* Node : AllNodes)
	{
		if (!Node)
		{
			continue;
		}

		TSharedRef<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());

		if (Node->ComponentClass)
		{
			CompObj->SetStringField(TEXT("componentClass"), Node->ComponentClass->GetName());
		}
		else
		{
			CompObj->SetStringField(TEXT("componentClass"), TEXT("None"));
		}

		// Parent component info
		USCS_Node* ParentNode = nullptr;
		for (USCS_Node* Candidate : AllNodes)
		{
			if (Candidate && Candidate->GetChildNodes().Contains(Node))
			{
				ParentNode = Candidate;
				break;
			}
		}

		if (ParentNode)
		{
			CompObj->SetStringField(TEXT("parentComponent"), ParentNode->GetVariableName().ToString());
		}

		// Check if this is a default scene root (first root node with SceneComponent class)
		bool bIsSceneRoot = false;
		const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
		if (RootNodes.Num() > 0 && RootNodes[0] == Node)
		{
			bIsSceneRoot = true;
		}
		CompObj->SetBoolField(TEXT("isSceneRoot"), bIsSceneRoot);

		// List child count for informational purposes
		CompObj->SetNumberField(TEXT("childCount"), Node->GetChildNodes().Num());

		ComponentsArr.Add(MakeShared<FJsonValueObject>(CompObj));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetNumberField(TEXT("count"), ComponentsArr.Num());
	Result->SetArrayField(TEXT("components"), ComponentsArr);
	return JsonToString(Result);
}

// ============================================================
// HandleAddComponent — add a component to a Blueprint's SCS
// ============================================================

FString FBlueprintMCPServer::HandleAddComponent(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString ComponentClassName = Json->GetStringField(TEXT("componentClass"));
	FString ComponentName = Json->GetStringField(TEXT("name"));

	if (BlueprintName.IsEmpty() || ComponentClassName.IsEmpty() || ComponentName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, componentClass, name"));
	}

	FString ParentComponentName;
	if (Json->HasField(TEXT("parentComponent")))
	{
		ParentComponentName = Json->GetStringField(TEXT("parentComponent"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Blueprint '%s' does not have a SimpleConstructionScript (not an Actor Blueprint)"),
			*BlueprintName));
	}

	// Check for duplicate component names
	const TArray<USCS_Node*>& ExistingNodes = SCS->GetAllNodes();
	for (USCS_Node* Existing : ExistingNodes)
	{
		if (Existing && Existing->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			return MakeErrorJson(FString::Printf(
				TEXT("A component named '%s' already exists in Blueprint '%s'"),
				*ComponentName, *BlueprintName));
		}
	}

	// Resolve the component class by name
	// Try multiple name variants: exact name, with U prefix, without U prefix
	UClass* ComponentClass = nullptr;

	TArray<FString> NamesToTry;
	NamesToTry.Add(ComponentClassName);
	if (!ComponentClassName.StartsWith(TEXT("U")))
	{
		NamesToTry.Add(FString::Printf(TEXT("U%s"), *ComponentClassName));
	}
	else
	{
		// Also try without U prefix
		NamesToTry.Add(ComponentClassName.Mid(1));
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UActorComponent::StaticClass()))
		{
			continue;
		}

		FString ClassName = It->GetName();
		for (const FString& NameToTry : NamesToTry)
		{
			if (ClassName.Equals(NameToTry, ESearchCase::IgnoreCase))
			{
				ComponentClass = *It;
				break;
			}
		}

		if (ComponentClass)
		{
			break;
		}
	}

	if (!ComponentClass)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Component class '%s' not found or is not a subclass of UActorComponent. "
				"Common classes: StaticMeshComponent, SkeletalMeshComponent, AudioComponent, "
				"SceneComponent, BoxCollisionComponent, SphereCollisionComponent, CapsuleComponent, "
				"ArrowComponent, ChildActorComponent, SpotLightComponent, PointLightComponent, "
				"WidgetComponent, BillboardComponent"),
			*ComponentClassName));
	}

	// If parent component specified, find its SCS node
	USCS_Node* ParentSCSNode = nullptr;
	if (!ParentComponentName.IsEmpty())
	{
		for (USCS_Node* Node : ExistingNodes)
		{
			if (Node && Node->GetVariableName().ToString().Equals(ParentComponentName, ESearchCase::IgnoreCase))
			{
				ParentSCSNode = Node;
				break;
			}
		}

		if (!ParentSCSNode)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Parent component '%s' not found in Blueprint '%s'"),
				*ParentComponentName, *BlueprintName));
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding component '%s' (%s) to Blueprint '%s'"),
		*ComponentName, *ComponentClass->GetName(), *BlueprintName);

	// Create the SCS node
	USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
	if (!NewNode)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to create SCS node for component '%s' with class '%s'"),
			*ComponentName, *ComponentClass->GetName()));
	}

	// Add to the hierarchy
	if (ParentSCSNode)
	{
		ParentSCSNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added component '%s' (%s) to '%s' (parent: %s, saved: %s)"),
		*ComponentName, *ComponentClass->GetName(), *BlueprintName,
		ParentSCSNode ? *ParentComponentName : TEXT("(root)"),
		bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("name"), NewNode->GetVariableName().ToString());
	Result->SetStringField(TEXT("componentClass"), ComponentClass->GetName());
	if (ParentSCSNode)
	{
		Result->SetStringField(TEXT("parentComponent"), ParentSCSNode->GetVariableName().ToString());
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveComponent — remove a component from a Blueprint's SCS
// ============================================================

FString FBlueprintMCPServer::HandleRemoveComponent(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString ComponentName = Json->GetStringField(TEXT("name"));

	if (BlueprintName.IsEmpty() || ComponentName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, name"));
	}

	FString LoadError;
	UBlueprint* BP = LoadBlueprintByName(BlueprintName, LoadError);
	if (!BP)
	{
		return MakeErrorJson(LoadError);
	}

	USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Blueprint '%s' does not have a SimpleConstructionScript (not an Actor Blueprint)"),
			*BlueprintName));
	}

	// Find the node to remove
	USCS_Node* NodeToRemove = nullptr;
	const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
	for (USCS_Node* Node : AllNodes)
	{
		if (Node && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			NodeToRemove = Node;
			break;
		}
	}

	if (!NodeToRemove)
	{
		// Build list of component names for the error message
		TArray<TSharedPtr<FJsonValue>> CompList;
		for (USCS_Node* Node : AllNodes)
		{
			if (Node)
			{
				CompList.Add(MakeShared<FJsonValueString>(Node->GetVariableName().ToString()));
			}
		}

		TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Component '%s' not found in Blueprint '%s'"),
			*ComponentName, *BlueprintName));
		ErrorResult->SetArrayField(TEXT("existingComponents"), CompList);
		return JsonToString(ErrorResult);
	}

	// Prevent removing the root scene component if it has children
	const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
	if (RootNodes.Contains(NodeToRemove) && NodeToRemove->GetChildNodes().Num() > 0)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot remove component '%s' because it is a root component with %d child(ren). "
				"Remove or re-parent the children first."),
			*ComponentName, NodeToRemove->GetChildNodes().Num()));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removing component '%s' from Blueprint '%s'"),
		*ComponentName, *BlueprintName);

	// Remove the node (promotes children to parent if it has any — but we've guarded root above)
	SCS->RemoveNodeAndPromoteChildren(NodeToRemove);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	bool bSaved = SaveBlueprintPackage(BP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed component '%s' from '%s' (saved: %s)"),
		*ComponentName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("name"), ComponentName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
