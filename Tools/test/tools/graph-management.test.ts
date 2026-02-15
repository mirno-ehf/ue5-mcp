import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("delete_graph / rename_graph", () => {
  const bpName = uniqueName("BP_GraphMgmtTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Create a function graph to test with
    const fn = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyTestFunction",
      graphType: "function",
    });
    expect(fn.error).toBeUndefined();

    // Create a macro graph to test with
    const macro = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "MyTestMacro",
      graphType: "macro",
    });
    expect(macro.error).toBeUndefined();

    // Create another function for rename testing
    const fn2 = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "FunctionToRename",
      graphType: "function",
    });
    expect(fn2.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // --- rename_graph tests ---

  it("renames a function graph", async () => {
    const data = await uePost("/api/rename-graph", {
      blueprint: bpName,
      graphName: "FunctionToRename",
      newName: "RenamedFunction",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.oldName).toBe("FunctionToRename");
    expect(data.newName).toBe("RenamedFunction");
    expect(data.graphType).toBe("function");
    expect(data.saved).toBe(true);
  });

  it("verifies renamed graph appears in blueprint", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    const graphNames = data.graphs.map((g: any) => g.name || g);
    expect(graphNames).toContain("RenamedFunction");
    expect(graphNames).not.toContain("FunctionToRename");
  });

  it("rejects renaming EventGraph", async () => {
    const data = await uePost("/api/rename-graph", {
      blueprint: bpName,
      graphName: "EventGraph",
      newName: "MyEventGraph",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("Cannot rename");
  });

  it("rejects rename to existing name", async () => {
    const data = await uePost("/api/rename-graph", {
      blueprint: bpName,
      graphName: "RenamedFunction",
      newName: "MyTestMacro",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects rename of non-existent graph", async () => {
    const data = await uePost("/api/rename-graph", {
      blueprint: bpName,
      graphName: "NonExistentGraph_XYZ",
      newName: "NewName",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields for rename", async () => {
    const data = await uePost("/api/rename-graph", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  // --- delete_graph tests ---

  it("deletes a function graph", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: bpName,
      graphName: "MyTestFunction",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphName).toBe("MyTestFunction");
    expect(data.graphType).toBe("function");
    expect(data.nodeCount).toBeDefined();
    expect(data.saved).toBe(true);
  });

  it("verifies deleted graph is gone", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    const graphNames = data.graphs.map((g: any) => g.name || g);
    expect(graphNames).not.toContain("MyTestFunction");
  });

  it("deletes a macro graph", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: bpName,
      graphName: "MyTestMacro",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphType).toBe("macro");
    expect(data.saved).toBe(true);
  });

  it("rejects deleting EventGraph", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: bpName,
      graphName: "EventGraph",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("Cannot delete");
  });

  it("rejects deleting non-existent graph", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: bpName,
      graphName: "NonExistentGraph_XYZ",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields for delete", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/delete-graph", {
      blueprint: "BP_Nonexistent_XYZ_999",
      graphName: "SomeGraph",
    });
    expect(data.error).toBeDefined();
  });
});
