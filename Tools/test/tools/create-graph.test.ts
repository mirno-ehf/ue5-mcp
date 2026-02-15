import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("create_graph", () => {
  const bpName = uniqueName("BP_CreateGraphTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("creates a function graph", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyFunction",
      graphType: "function",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphName).toBe("MyFunction");
    expect(data.graphType).toBe("function");
    expect(data.saved).toBe(true);
  });

  it("function graph appears in get_blueprint", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    const graphNames = data.graphs.map((g: any) => g.name);
    expect(graphNames).toContain("MyFunction");
  });

  it("creates a macro graph", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyMacro",
      graphType: "macro",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphName).toBe("MyMacro");
    expect(data.graphType).toBe("macro");
    expect(data.saved).toBe(true);
  });

  it("creates a custom event", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyCustomEvent",
      graphType: "customEvent",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphName).toBe("MyCustomEvent");
    expect(data.graphType).toBe("customEvent");
    expect(data.nodeId).toBeDefined();
    expect(data.saved).toBe(true);
  });

  it("rejects duplicate graph name", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyFunction",
      graphType: "function",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects duplicate custom event name", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyCustomEvent",
      graphType: "customEvent",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: "BP_Nonexistent_XYZ_999",
      graphName: "SomeGraph",
      graphType: "function",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects invalid graphType", async () => {
    const data = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "BadType",
      graphType: "invalid",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("Invalid graphType");
  });

  it("can add a node to the new function graph", async () => {
    const data = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "MyFunction",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.nodeId).toBeDefined();
  });
});
