#include "BlueprintMCPServer.h"
#include "Engine/Blueprint.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Overlay.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/GridPanel.h"
#include "Components/WrapBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/Spacer.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "UObject/PropertyIterator.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "WidgetBlueprintFactory.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

// ============================================================
// LoadWidgetBlueprintByName — load and cast to UWidgetBlueprint
// ============================================================

UWidgetBlueprint* FBlueprintMCPServer::LoadWidgetBlueprintByName(const FString& NameOrPath, FString& OutError)
{
	UBlueprint* BP = LoadBlueprintByName(NameOrPath, OutError);
	if (!BP)
	{
		return nullptr;
	}

	UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(BP);
	if (!WidgetBP)
	{
		OutError = FString::Printf(
			TEXT("Blueprint '%s' is not a Widget Blueprint (class: %s). "
				"This tool only works on Widget Blueprints (UMG)."),
			*NameOrPath, *BP->GetClass()->GetName());
		return nullptr;
	}

	return WidgetBP;
}

// ============================================================
// Helper: Find a widget by name in the widget tree
// ============================================================

static UWidget* FindWidgetByName(UWidgetTree* Tree, const FString& WidgetName)
{
	TArray<UWidget*> AllWidgets;
	Tree->GetAllWidgets(AllWidgets);

	// Exact match first
	for (UWidget* W : AllWidgets)
	{
		if (W && W->GetName() == WidgetName)
		{
			return W;
		}
	}

	// Case-insensitive fallback
	for (UWidget* W : AllWidgets)
	{
		if (W && W->GetName().Equals(WidgetName, ESearchCase::IgnoreCase))
		{
			return W;
		}
	}

	return nullptr;
}

// ============================================================
// Helper: Check if Target is a descendant of Ancestor
// ============================================================

static bool IsDescendantOf(UWidget* Target, UWidget* Ancestor)
{
	if (!Target || !Ancestor)
	{
		return false;
	}

	UPanelWidget* Panel = Cast<UPanelWidget>(Ancestor);
	if (!Panel)
	{
		return false;
	}

	for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
	{
		UWidget* Child = Panel->GetChildAt(i);
		if (Child == Target)
		{
			return true;
		}
		if (IsDescendantOf(Target, Child))
		{
			return true;
		}
	}

	return false;
}

// ============================================================
// Helper: Serialize a widget to JSON (for tree listing)
// ============================================================

static TSharedRef<FJsonObject> SerializeWidgetInfo(UWidget* Widget, UPanelWidget* ParentPanel)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("name"), Widget->GetName());
	Obj->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

	if (ParentPanel)
	{
		Obj->SetStringField(TEXT("parent"), ParentPanel->GetName());
	}

	// Slot info
	UPanelSlot* Slot = Widget->Slot;
	if (Slot)
	{
		Obj->SetStringField(TEXT("slotClass"), Slot->GetClass()->GetName());
	}

	// Visibility
	Obj->SetStringField(TEXT("visibility"), StaticEnum<ESlateVisibility>()->GetNameStringByValue((int64)Widget->GetVisibility()));

	// If this widget is a panel, list child count
	UPanelWidget* AsPanel = Cast<UPanelWidget>(Widget);
	if (AsPanel)
	{
		Obj->SetBoolField(TEXT("isPanel"), true);
		Obj->SetNumberField(TEXT("childCount"), AsPanel->GetChildrenCount());
	}
	else
	{
		Obj->SetBoolField(TEXT("isPanel"), false);
	}

	return Obj;
}

// ============================================================
// Helper: Build widget tree recursively
// ============================================================

static void BuildWidgetTreeRecursive(UWidget* Widget, UPanelWidget* ParentPanel, TArray<TSharedPtr<FJsonValue>>& OutArray, int32 Depth)
{
	if (!Widget)
	{
		return;
	}

	TSharedRef<FJsonObject> WidgetObj = SerializeWidgetInfo(Widget, ParentPanel);
	WidgetObj->SetNumberField(TEXT("depth"), Depth);
	OutArray.Add(MakeShared<FJsonValueObject>(WidgetObj));

	UPanelWidget* Panel = Cast<UPanelWidget>(Widget);
	if (Panel)
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			BuildWidgetTreeRecursive(Panel->GetChildAt(i), Panel, OutArray, Depth + 1);
		}
	}
}

// ============================================================
// HandleListWidgetTree
// ============================================================

FString FBlueprintMCPServer::HandleListWidgetTree(const FString& Body)
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
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget Blueprint '%s' has no WidgetTree"),
			*BlueprintName));
	}

	TArray<TSharedPtr<FJsonValue>> WidgetsArr;

	UWidget* Root = Tree->RootWidget;
	if (Root)
	{
		BuildWidgetTreeRecursive(Root, nullptr, WidgetsArr, 0);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("rootWidget"), Root ? Root->GetName() : TEXT("(none)"));
	Result->SetNumberField(TEXT("count"), WidgetsArr.Num());
	Result->SetArrayField(TEXT("widgets"), WidgetsArr);
	return JsonToString(Result);
}

// ============================================================
// HandleGetWidgetProperties
// ============================================================

FString FBlueprintMCPServer::HandleGetWidgetProperties(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString WidgetName = Json->GetStringField(TEXT("widget"));

	if (BlueprintName.IsEmpty() || WidgetName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, widget"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(TEXT("Widget Blueprint has no WidgetTree"));
	}

	UWidget* Widget = FindWidgetByName(Tree, WidgetName);
	if (!Widget)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget '%s' not found in Widget Blueprint '%s'"),
			*WidgetName, *BlueprintName));
	}

	// Collect editable properties from the widget
	TArray<TSharedPtr<FJsonValue>> PropsArr;

	for (TFieldIterator<FProperty> It(Widget->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Prop->GetName());
		PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
		PropObj->SetStringField(TEXT("source"), TEXT("widget"));

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
		Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
		PropObj->SetStringField(TEXT("value"), ValueStr);

		PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
	}

	// Collect slot properties if the widget has a slot
	UPanelSlot* Slot = Widget->Slot;
	if (Slot)
	{
		for (TFieldIterator<FProperty> It(Slot->GetClass()); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			TSharedRef<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), Prop->GetName());
			PropObj->SetStringField(TEXT("type"), Prop->GetCPPType());
			PropObj->SetStringField(TEXT("source"), TEXT("slot"));

			FString ValueStr;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Slot);
			Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, nullptr, PPF_None);
			PropObj->SetStringField(TEXT("value"), ValueStr);

			PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("widget"), Widget->GetName());
	Result->SetStringField(TEXT("widgetClass"), Widget->GetClass()->GetName());
	if (Slot)
	{
		Result->SetStringField(TEXT("slotClass"), Slot->GetClass()->GetName());
	}
	Result->SetNumberField(TEXT("propertyCount"), PropsArr.Num());
	Result->SetArrayField(TEXT("properties"), PropsArr);
	return JsonToString(Result);
}

// ============================================================
// HandleAddWidget
// ============================================================

FString FBlueprintMCPServer::HandleAddWidget(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString WidgetClassName = Json->GetStringField(TEXT("widgetClass"));
	FString WidgetName = Json->GetStringField(TEXT("name"));

	if (BlueprintName.IsEmpty() || WidgetClassName.IsEmpty() || WidgetName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, widgetClass, name"));
	}

	FString ParentWidgetName;
	if (Json->HasField(TEXT("parent")))
	{
		ParentWidgetName = Json->GetStringField(TEXT("parent"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(TEXT("Widget Blueprint has no WidgetTree"));
	}

	// Check for duplicate widget names
	if (FindWidgetByName(Tree, WidgetName))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("A widget named '%s' already exists in Widget Blueprint '%s'"),
			*WidgetName, *BlueprintName));
	}

	// Resolve the widget class
	UClass* WidgetClass = nullptr;
	TArray<FString> NamesToTry;
	NamesToTry.Add(WidgetClassName);
	if (!WidgetClassName.StartsWith(TEXT("U")))
	{
		NamesToTry.Add(FString::Printf(TEXT("U%s"), *WidgetClassName));
	}
	else
	{
		NamesToTry.Add(WidgetClassName.Mid(1));
	}

	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}

		FString ClassName = It->GetName();
		for (const FString& NameToTry : NamesToTry)
		{
			if (ClassName.Equals(NameToTry, ESearchCase::IgnoreCase))
			{
				WidgetClass = *It;
				break;
			}
		}
		if (WidgetClass)
		{
			break;
		}
	}

	if (!WidgetClass)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget class '%s' not found or is not a subclass of UWidget. "
				"Common classes: TextBlock, Button, Image, VerticalBox, HorizontalBox, "
				"Overlay, CanvasPanel, Border, SizeBox, ScaleBox, ScrollBox, GridPanel, "
				"WrapBox, WidgetSwitcher, Spacer, ProgressBar, Slider, CheckBox, "
				"ComboBoxString, EditableTextBox"),
			*WidgetClassName));
	}

	// Find parent panel if specified
	UPanelWidget* ParentPanel = nullptr;
	if (!ParentWidgetName.IsEmpty())
	{
		UWidget* ParentWidget = FindWidgetByName(Tree, ParentWidgetName);
		if (!ParentWidget)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Parent widget '%s' not found in Widget Blueprint '%s'"),
				*ParentWidgetName, *BlueprintName));
		}

		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Parent widget '%s' (class: %s) is not a panel widget and cannot have children. "
					"Only panel widgets (CanvasPanel, VerticalBox, HorizontalBox, Overlay, etc.) "
					"can contain child widgets."),
				*ParentWidgetName, *ParentWidget->GetClass()->GetName()));
		}
	}
	else
	{
		// If no parent specified and root exists, try to add to the root if it's a panel
		UWidget* Root = Tree->RootWidget;
		if (Root)
		{
			ParentPanel = Cast<UPanelWidget>(Root);
			if (!ParentPanel)
			{
				return MakeErrorJson(FString::Printf(
					TEXT("The root widget '%s' (class: %s) is not a panel widget. "
						"Specify a 'parent' panel widget explicitly, or set this widget as root by first removing the existing root."),
					*Root->GetName(), *Root->GetClass()->GetName()));
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Adding widget '%s' (%s) to Widget Blueprint '%s'"),
		*WidgetName, *WidgetClass->GetName(), *BlueprintName);

	// Create the widget in the tree
	UWidget* NewWidget = Tree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
	if (!NewWidget)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to construct widget '%s' with class '%s'"),
			*WidgetName, *WidgetClass->GetName()));
	}

	// Add to parent or set as root
	if (ParentPanel)
	{
		UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
		if (!Slot)
		{
			return MakeErrorJson(FString::Printf(
				TEXT("Failed to add widget '%s' as child of '%s'"),
				*WidgetName, *ParentPanel->GetName()));
		}
	}
	else
	{
		// No root widget exists — set this as root
		Tree->RootWidget = NewWidget;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	bool bSaved = SaveBlueprintPackage(WidgetBP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Added widget '%s' (%s) to '%s' (parent: %s, saved: %s)"),
		*WidgetName, *WidgetClass->GetName(), *BlueprintName,
		ParentPanel ? *ParentPanel->GetName() : TEXT("(root)"),
		bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("name"), NewWidget->GetName());
	Result->SetStringField(TEXT("widgetClass"), WidgetClass->GetName());
	if (ParentPanel)
	{
		Result->SetStringField(TEXT("parent"), ParentPanel->GetName());
	}
	else
	{
		Result->SetStringField(TEXT("parent"), TEXT("(root)"));
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleRemoveWidget
// ============================================================

FString FBlueprintMCPServer::HandleRemoveWidget(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString WidgetName = Json->GetStringField(TEXT("widget"));

	if (BlueprintName.IsEmpty() || WidgetName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, widget"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(TEXT("Widget Blueprint has no WidgetTree"));
	}

	UWidget* Widget = FindWidgetByName(Tree, WidgetName);
	if (!Widget)
	{
		// Build list of widget names for error message
		TArray<UWidget*> AllWidgets;
		Tree->GetAllWidgets(AllWidgets);
		TArray<TSharedPtr<FJsonValue>> NameList;
		for (UWidget* W : AllWidgets)
		{
			if (W)
			{
				NameList.Add(MakeShared<FJsonValueString>(W->GetName()));
			}
		}

		TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("error"), FString::Printf(
			TEXT("Widget '%s' not found in Widget Blueprint '%s'"),
			*WidgetName, *BlueprintName));
		ErrorResult->SetArrayField(TEXT("existingWidgets"), NameList);
		return JsonToString(ErrorResult);
	}

	// Refuse to remove panels that have children
	UPanelWidget* AsPanel = Cast<UPanelWidget>(Widget);
	if (AsPanel && AsPanel->GetChildrenCount() > 0)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot remove widget '%s' because it is a panel with %d child(ren). "
				"Remove or move the children first."),
			*WidgetName, AsPanel->GetChildrenCount()));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removing widget '%s' from Widget Blueprint '%s'"),
		*WidgetName, *BlueprintName);

	// Remove from parent panel
	UPanelWidget* ParentPanel = Widget->GetParent();
	if (ParentPanel)
	{
		ParentPanel->RemoveChild(Widget);
	}
	else if (Tree->RootWidget == Widget)
	{
		Tree->RootWidget = nullptr;
	}

	// Remove from widget tree
	Tree->RemoveWidget(Widget);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	bool bSaved = SaveBlueprintPackage(WidgetBP);

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Removed widget '%s' from '%s' (saved: %s)"),
		*WidgetName, *BlueprintName, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("widget"), WidgetName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleSetWidgetProperty
// ============================================================

FString FBlueprintMCPServer::HandleSetWidgetProperty(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString WidgetName = Json->GetStringField(TEXT("widget"));
	FString PropertyName = Json->GetStringField(TEXT("property"));
	FString Value = Json->GetStringField(TEXT("value"));

	if (BlueprintName.IsEmpty() || WidgetName.IsEmpty() || PropertyName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, widget, property"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(TEXT("Widget Blueprint has no WidgetTree"));
	}

	UWidget* Widget = FindWidgetByName(Tree, WidgetName);
	if (!Widget)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget '%s' not found in Widget Blueprint '%s'"),
			*WidgetName, *BlueprintName));
	}

	// Try to find the property on the widget first
	FProperty* Prop = Widget->GetClass()->FindPropertyByName(FName(*PropertyName));
	UObject* TargetObject = Widget;
	FString Source = TEXT("widget");

	// Fall through to slot if not found on widget
	if (!Prop && Widget->Slot)
	{
		Prop = Widget->Slot->GetClass()->FindPropertyByName(FName(*PropertyName));
		if (Prop)
		{
			TargetObject = Widget->Slot;
			Source = TEXT("slot");
		}
	}

	if (!Prop)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Property '%s' not found on widget '%s' (class: %s) or its slot. "
				"Use get_widget_properties to see all available properties."),
			*PropertyName, *WidgetName, *Widget->GetClass()->GetName()));
	}

	// Auto-wrap bare strings in INVTEXT("...") for FText properties
	FString ValueToSet = Value;
	FTextProperty* TextProp = CastField<FTextProperty>(Prop);
	if (TextProp && !Value.StartsWith(TEXT("INVTEXT(")) && !Value.StartsWith(TEXT("NSLOCTEXT(")))
	{
		ValueToSet = FString::Printf(TEXT("INVTEXT(\"%s\")"), *Value);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Setting property '%s' on widget '%s' to '%s' (source: %s)"),
		*PropertyName, *WidgetName, *ValueToSet, *Source);

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetObject);
	const TCHAR* ImportResult = Prop->ImportText_Direct(*ValueToSet, ValuePtr, TargetObject, PPF_None);

	if (!ImportResult)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to set property '%s' to value '%s' on widget '%s'. "
				"The value may be in an incorrect format for type '%s'."),
			*PropertyName, *Value, *WidgetName, *Prop->GetCPPType()));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	bool bSaved = SaveBlueprintPackage(WidgetBP);

	// Read back the value
	FString ActualValue;
	Prop->ExportTextItem_Direct(ActualValue, ValuePtr, nullptr, nullptr, PPF_None);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("widget"), WidgetName);
	Result->SetStringField(TEXT("property"), PropertyName);
	Result->SetStringField(TEXT("value"), ActualValue);
	Result->SetStringField(TEXT("source"), Source);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleMoveWidget
// ============================================================

FString FBlueprintMCPServer::HandleMoveWidget(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString BlueprintName = Json->GetStringField(TEXT("blueprint"));
	FString WidgetName = Json->GetStringField(TEXT("widget"));
	FString NewParentName = Json->GetStringField(TEXT("newParent"));

	if (BlueprintName.IsEmpty() || WidgetName.IsEmpty() || NewParentName.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required fields: blueprint, widget, newParent"));
	}

	FString LoadError;
	UWidgetBlueprint* WidgetBP = LoadWidgetBlueprintByName(BlueprintName, LoadError);
	if (!WidgetBP)
	{
		return MakeErrorJson(LoadError);
	}

	UWidgetTree* Tree = WidgetBP->WidgetTree;
	if (!Tree)
	{
		return MakeErrorJson(TEXT("Widget Blueprint has no WidgetTree"));
	}

	UWidget* Widget = FindWidgetByName(Tree, WidgetName);
	if (!Widget)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget '%s' not found in Widget Blueprint '%s'"),
			*WidgetName, *BlueprintName));
	}

	UWidget* NewParentWidget = FindWidgetByName(Tree, NewParentName);
	if (!NewParentWidget)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Target parent widget '%s' not found in Widget Blueprint '%s'"),
			*NewParentName, *BlueprintName));
	}

	UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParentWidget);
	if (!NewParentPanel)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Target parent '%s' (class: %s) is not a panel widget and cannot have children."),
			*NewParentName, *NewParentWidget->GetClass()->GetName()));
	}

	// Cycle detection: refuse to move widget into its own descendant
	if (IsDescendantOf(NewParentWidget, Widget))
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Cannot move widget '%s' into '%s' because '%s' is a descendant of '%s'. "
				"This would create a cycle in the widget tree."),
			*WidgetName, *NewParentName, *NewParentName, *WidgetName));
	}

	// Cannot move to same parent
	if (Widget->GetParent() == NewParentPanel)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Widget '%s' is already a child of '%s'"),
			*WidgetName, *NewParentName));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Moving widget '%s' to new parent '%s' in '%s'"),
		*WidgetName, *NewParentName, *BlueprintName);

	// Remove from current parent
	UPanelWidget* OldParent = Widget->GetParent();
	FString OldParentName = OldParent ? OldParent->GetName() : TEXT("(root)");
	if (OldParent)
	{
		OldParent->RemoveChild(Widget);
	}
	else if (Tree->RootWidget == Widget)
	{
		Tree->RootWidget = nullptr;
	}

	// Add to new parent
	UPanelSlot* NewSlot = NewParentPanel->AddChild(Widget);
	if (!NewSlot)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to add widget '%s' as child of '%s'"),
			*WidgetName, *NewParentName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
	bool bSaved = SaveBlueprintPackage(WidgetBP);

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("blueprint"), BlueprintName);
	Result->SetStringField(TEXT("widget"), WidgetName);
	Result->SetStringField(TEXT("oldParent"), OldParentName);
	Result->SetStringField(TEXT("newParent"), NewParentName);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}

// ============================================================
// HandleCreateWidgetBlueprint
// ============================================================

FString FBlueprintMCPServer::HandleCreateWidgetBlueprint(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body"));
	}

	FString Name = Json->GetStringField(TEXT("name"));
	FString PackagePath = Json->GetStringField(TEXT("packagePath"));

	if (Name.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: name"));
	}
	if (PackagePath.IsEmpty())
	{
		PackagePath = TEXT("/Game");
	}

	// Check if asset already exists
	FString FullPath = PackagePath / Name;
	FAssetData* Existing = FindBlueprintAsset(FullPath);
	if (!Existing)
	{
		Existing = FindBlueprintAsset(Name);
	}
	if (Existing)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("An asset named '%s' already exists at '%s'"),
			*Name, *Existing->PackageName.ToString()));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Creating Widget Blueprint '%s' at '%s'"),
		*Name, *PackagePath);

	// Create the Widget Blueprint using the factory
	UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UWidgetBlueprint::StaticClass(), Factory);

	if (!NewAsset)
	{
		return MakeErrorJson(FString::Printf(
			TEXT("Failed to create Widget Blueprint '%s' at '%s'"),
			*Name, *PackagePath));
	}

	UWidgetBlueprint* NewWidgetBP = Cast<UWidgetBlueprint>(NewAsset);
	if (!NewWidgetBP)
	{
		return MakeErrorJson(TEXT("Created asset is not a Widget Blueprint"));
	}

	// Save the package
	bool bSaved = SaveBlueprintPackage(NewWidgetBP);

	// Add to our cached asset list
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	FAssetData NewAssetData = ARM.Get().GetAssetByObjectPath(FSoftObjectPath(NewWidgetBP));
	if (NewAssetData.IsValid())
	{
		AllBlueprintAssets.Add(NewAssetData);
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: Created Widget Blueprint '%s' (saved: %s)"),
		*Name, bSaved ? TEXT("true") : TEXT("false"));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("name"), Name);
	Result->SetStringField(TEXT("packagePath"), PackagePath);
	Result->SetStringField(TEXT("fullPath"), FullPath);
	Result->SetStringField(TEXT("class"), TEXT("WidgetBlueprint"));
	Result->SetBoolField(TEXT("saved"), bSaved);
	return JsonToString(Result);
}
