import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestWidgetBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("Widget Blueprint tools", () => {
  const wbpName = uniqueName("WBP_WidgetTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestWidgetBlueprint({ name: wbpName });
    expect(res.error).toBeUndefined();
    expect(res.success).toBe(true);
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${wbpName}`);
  });

  // --- create_widget_blueprint tests ---

  it("create_widget_blueprint succeeds", async () => {
    const name = uniqueName("WBP_CreateTest");
    const data = await uePost("/api/create-widget-blueprint", {
      name,
      packagePath,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.name).toBe(name);
    expect(data.class).toBe("WidgetBlueprint");

    // Cleanup
    await deleteTestBlueprint(`${packagePath}/${name}`);
  });

  it("create_widget_blueprint rejects missing name", async () => {
    const data = await uePost("/api/create-widget-blueprint", {});
    expect(data.error).toBeDefined();
  });

  // --- list_widget_tree tests ---

  it("list_widget_tree on empty widget blueprint", async () => {
    const data = await uePost("/api/list-widget-tree", { blueprint: wbpName });
    expect(data.error).toBeUndefined();
    expect(data.blueprint).toBe(wbpName);
    expect(data.widgets).toBeDefined();
    expect(Array.isArray(data.widgets)).toBe(true);
  });

  it("list_widget_tree rejects non-widget blueprint", async () => {
    // Create a regular Actor blueprint
    const actorBpName = uniqueName("BP_NonWidgetTest");
    await uePost("/api/create-blueprint", {
      blueprintName: actorBpName,
      packagePath,
      parentClass: "Actor",
      blueprintType: "Normal",
    });

    const data = await uePost("/api/list-widget-tree", { blueprint: actorBpName });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not a Widget Blueprint");

    await deleteTestBlueprint(`${packagePath}/${actorBpName}`);
  });

  it("list_widget_tree rejects missing fields", async () => {
    const data = await uePost("/api/list-widget-tree", {});
    expect(data.error).toBeDefined();
  });

  it("list_widget_tree rejects non-existent blueprint", async () => {
    const data = await uePost("/api/list-widget-tree", { blueprint: "WBP_Nonexistent_XYZ_999" });
    expect(data.error).toBeDefined();
  });

  // --- add_widget tests ---

  it("adds a CanvasPanel as root", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "CanvasPanel",
      name: "RootCanvas",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.name).toBe("RootCanvas");
    expect(data.widgetClass).toContain("CanvasPanel");
    expect(data.saved).toBe(true);
  });

  it("lists the root CanvasPanel", async () => {
    const data = await uePost("/api/list-widget-tree", { blueprint: wbpName });
    expect(data.error).toBeUndefined();
    expect(data.rootWidget).toBe("RootCanvas");
    expect(data.count).toBeGreaterThanOrEqual(1);
    const root = data.widgets.find((w: any) => w.name === "RootCanvas");
    expect(root).toBeDefined();
    expect(root.isPanel).toBe(true);
  });

  it("adds a TextBlock to the CanvasPanel", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "TextBlock",
      name: "TitleText",
      parent: "RootCanvas",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.name).toBe("TitleText");
    expect(data.parent).toBe("RootCanvas");
  });

  it("adds a VerticalBox to the CanvasPanel", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "VerticalBox",
      name: "MainVBox",
      parent: "RootCanvas",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.widgetClass).toContain("VerticalBox");
  });

  it("adds a Button inside the VerticalBox", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "Button",
      name: "StartButton",
      parent: "MainVBox",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.parent).toBe("MainVBox");
  });

  it("rejects duplicate widget name", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "TextBlock",
      name: "TitleText",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects adding child to non-panel widget", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "TextBlock",
      name: "ChildOfText",
      parent: "TitleText",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not a panel");
  });

  it("rejects invalid widget class", async () => {
    const data = await uePost("/api/add-widget", {
      blueprint: wbpName,
      widgetClass: "FakeWidgetClass_XYZ_999",
      name: "TestFake",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects missing required fields for add_widget", async () => {
    const data = await uePost("/api/add-widget", {});
    expect(data.error).toBeDefined();
  });

  // --- get_widget_properties tests ---

  it("gets properties for a TextBlock", async () => {
    const data = await uePost("/api/get-widget-properties", {
      blueprint: wbpName,
      widget: "TitleText",
    });
    expect(data.error).toBeUndefined();
    expect(data.widget).toBe("TitleText");
    expect(data.widgetClass).toContain("TextBlock");
    expect(data.properties).toBeDefined();
    expect(Array.isArray(data.properties)).toBe(true);
    expect(data.propertyCount).toBeGreaterThan(0);

    // Should have both widget and slot properties
    const widgetProps = data.properties.filter((p: any) => p.source === "widget");
    const slotProps = data.properties.filter((p: any) => p.source === "slot");
    expect(widgetProps.length).toBeGreaterThan(0);
    expect(slotProps.length).toBeGreaterThan(0);
  });

  it("rejects get_widget_properties for non-existent widget", async () => {
    const data = await uePost("/api/get-widget-properties", {
      blueprint: wbpName,
      widget: "NonExistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects get_widget_properties with missing fields", async () => {
    const data = await uePost("/api/get-widget-properties", {
      blueprint: wbpName,
    });
    expect(data.error).toBeDefined();
  });

  // --- set_widget_property tests ---

  it("sets Text property on a TextBlock", async () => {
    const data = await uePost("/api/set-widget-property", {
      blueprint: wbpName,
      widget: "TitleText",
      property: "Text",
      value: "Hello World",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.property).toBe("Text");
    expect(data.source).toBe("widget");
  });

  it("rejects set_widget_property for invalid property", async () => {
    const data = await uePost("/api/set-widget-property", {
      blueprint: wbpName,
      widget: "TitleText",
      property: "NonExistentProperty_XYZ_999",
      value: "test",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects set_widget_property with missing fields", async () => {
    const data = await uePost("/api/set-widget-property", {
      blueprint: wbpName,
      widget: "TitleText",
    });
    expect(data.error).toBeDefined();
  });

  // --- move_widget tests ---

  it("moves a widget to a different parent", async () => {
    // Move TitleText from RootCanvas to MainVBox
    const data = await uePost("/api/move-widget", {
      blueprint: wbpName,
      widget: "TitleText",
      newParent: "MainVBox",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.widget).toBe("TitleText");
    expect(data.oldParent).toBe("RootCanvas");
    expect(data.newParent).toBe("MainVBox");
  });

  it("verifies widget was moved", async () => {
    const data = await uePost("/api/list-widget-tree", { blueprint: wbpName });
    expect(data.error).toBeUndefined();
    const titleText = data.widgets.find((w: any) => w.name === "TitleText");
    expect(titleText).toBeDefined();
    expect(titleText.parent).toBe("MainVBox");
  });

  it("rejects move to non-panel target", async () => {
    const data = await uePost("/api/move-widget", {
      blueprint: wbpName,
      widget: "StartButton",
      newParent: "TitleText",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not a panel");
  });

  it("rejects move that would create a cycle", async () => {
    // Try to move MainVBox (which contains StartButton) into StartButton
    // StartButton is a PanelWidget (Button is a ContentWidget/PanelWidget)
    // but MainVBox is an ancestor of StartButton so this should fail
    const data = await uePost("/api/move-widget", {
      blueprint: wbpName,
      widget: "MainVBox",
      newParent: "StartButton",
    });
    expect(data.error).toBeDefined();
    // Could be cycle detection or non-panel error depending on Button's type
    expect(data.error).toBeTruthy();
  });

  it("rejects move with missing fields", async () => {
    const data = await uePost("/api/move-widget", {
      blueprint: wbpName,
      widget: "TitleText",
    });
    expect(data.error).toBeDefined();
  });

  // --- remove_widget tests ---

  it("removes a leaf widget (StartButton)", async () => {
    const data = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "StartButton",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.saved).toBe(true);
  });

  it("removes TitleText", async () => {
    const data = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "TitleText",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
  });

  it("verifies widgets are removed", async () => {
    const data = await uePost("/api/list-widget-tree", { blueprint: wbpName });
    expect(data.error).toBeUndefined();
    const btn = data.widgets.find((w: any) => w.name === "StartButton");
    expect(btn).toBeUndefined();
    const txt = data.widgets.find((w: any) => w.name === "TitleText");
    expect(txt).toBeUndefined();
  });

  it("rejects removing non-existent widget", async () => {
    const data = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "NonExistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
    expect(data.existingWidgets).toBeDefined();
  });

  it("rejects removing panel with children", async () => {
    // RootCanvas still has MainVBox as a child
    const data = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "RootCanvas",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("child");
  });

  it("rejects remove with missing fields", async () => {
    const data = await uePost("/api/remove-widget", {
      blueprint: wbpName,
    });
    expect(data.error).toBeDefined();
  });

  it("cleans up remaining widgets (MainVBox then RootCanvas)", async () => {
    // Remove MainVBox (now empty after TitleText and StartButton were removed)
    const res1 = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "MainVBox",
    });
    expect(res1.error).toBeUndefined();

    // Now RootCanvas should be removable
    const res2 = await uePost("/api/remove-widget", {
      blueprint: wbpName,
      widget: "RootCanvas",
    });
    expect(res2.error).toBeUndefined();
  });
});
