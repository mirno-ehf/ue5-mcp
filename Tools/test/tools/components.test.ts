import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("add_component / remove_component / list_components", () => {
  const bpName = uniqueName("BP_ComponentTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName, parentClass: "Actor" });
    expect(res.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // --- list_components tests ---

  it("lists components on an Actor blueprint", async () => {
    const data = await uePost("/api/list-components", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.blueprint).toBe(bpName);
    expect(data.components).toBeDefined();
    expect(Array.isArray(data.components)).toBe(true);
    // An Actor BP should have at least a DefaultSceneRoot
    expect(data.components.length).toBeGreaterThanOrEqual(1);
  });

  it("first component is the scene root", async () => {
    const data = await uePost("/api/list-components", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    const root = data.components.find((c: any) => c.isSceneRoot === true);
    expect(root).toBeDefined();
  });

  // --- add_component tests ---

  it("adds a StaticMeshComponent", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: bpName,
      componentClass: "StaticMeshComponent",
      name: "TestMesh",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.name).toBe("TestMesh");
    expect(data.componentClass).toContain("StaticMeshComponent");
    expect(data.saved).toBe(true);
  });

  it("lists the added component", async () => {
    const data = await uePost("/api/list-components", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    const found = data.components.find((c: any) => c.name === "TestMesh");
    expect(found).toBeDefined();
    expect(found.componentClass).toContain("StaticMeshComponent");
  });

  it("adds a component with a parent", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: bpName,
      componentClass: "SceneComponent",
      name: "ChildScene",
      parentComponent: "TestMesh",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.parentComponent).toBeDefined();
  });

  it("verifies parent-child relationship", async () => {
    const data = await uePost("/api/list-components", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    const child = data.components.find((c: any) => c.name === "ChildScene");
    expect(child).toBeDefined();
    expect(child.parentComponent).toBe("TestMesh");
  });

  it("rejects duplicate component name", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: bpName,
      componentClass: "SceneComponent",
      name: "TestMesh",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects non-existent component class", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: bpName,
      componentClass: "FakeComponentClass_XYZ_999",
      name: "TestFake",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects non-existent parent component", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: bpName,
      componentClass: "SceneComponent",
      name: "TestOrphan",
      parentComponent: "NonExistentParent_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/add-component", {});
    expect(data.error).toBeDefined();
  });

  it("rejects add on non-existent blueprint", async () => {
    const data = await uePost("/api/add-component", {
      blueprint: "BP_Nonexistent_XYZ_999",
      componentClass: "StaticMeshComponent",
      name: "Test",
    });
    expect(data.error).toBeDefined();
  });

  // --- remove_component tests ---

  it("removes the child component first", async () => {
    const data = await uePost("/api/remove-component", {
      blueprint: bpName,
      name: "ChildScene",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.saved).toBe(true);
  });

  it("removes the TestMesh component", async () => {
    const data = await uePost("/api/remove-component", {
      blueprint: bpName,
      name: "TestMesh",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.saved).toBe(true);
  });

  it("verifies components are removed", async () => {
    const data = await uePost("/api/list-components", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    const mesh = data.components.find((c: any) => c.name === "TestMesh");
    expect(mesh).toBeUndefined();
    const child = data.components.find((c: any) => c.name === "ChildScene");
    expect(child).toBeUndefined();
  });

  it("rejects removing non-existent component", async () => {
    const data = await uePost("/api/remove-component", {
      blueprint: bpName,
      name: "NonExistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
    expect(data.existingComponents).toBeDefined();
  });

  it("rejects remove with missing required fields", async () => {
    const data = await uePost("/api/remove-component", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects remove on non-existent blueprint", async () => {
    const data = await uePost("/api/remove-component", {
      blueprint: "BP_Nonexistent_XYZ_999",
      name: "SomeComponent",
    });
    expect(data.error).toBeDefined();
  });

  // --- list_components error cases ---

  it("rejects list on non-existent blueprint", async () => {
    const data = await uePost("/api/list-components", {
      blueprint: "BP_Nonexistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects list with missing required fields", async () => {
    const data = await uePost("/api/list-components", {});
    expect(data.error).toBeDefined();
  });
});
