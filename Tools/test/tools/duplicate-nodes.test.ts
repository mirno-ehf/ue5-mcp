import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("duplicate_nodes", () => {
  const bpName = uniqueName("BP_DuplicateNodesTest");
  const packagePath = "/Game/Test";
  let printNodeId: string;
  let branchNodeId: string;

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    const n1 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
      posX: 100,
      posY: 100,
    });
    expect(n1.success).toBe(true);
    printNodeId = n1.nodeId;

    const n2 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "Branch",
      posX: 300,
      posY: 100,
    });
    expect(n2.success).toBe(true);
    branchNodeId = n2.nodeId;
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("duplicates a single node", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeIds: [printNodeId],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.duplicatedCount).toBe(1);
    expect(data.nodes).toHaveLength(1);
    expect(data.nodes[0].newNodeId).toBeDefined();
    expect(data.nodes[0].sourceNodeId).toBe(printNodeId);
    expect(data.saved).toBe(true);
  });

  it("duplicates multiple nodes", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeIds: [printNodeId, branchNodeId],
      offsetX: 200,
      offsetY: 100,
    });
    expect(data.error).toBeUndefined();
    expect(data.duplicatedCount).toBe(2);
    expect(data.nodes).toHaveLength(2);
    expect(data.saved).toBe(true);
  });

  it("duplicated nodes appear in graph", async () => {
    const graph = await ueGet("/api/graph", { name: bpName, graph: "EventGraph" });
    expect(graph.error).toBeUndefined();
    // Should have original nodes plus duplicated ones
    expect(graph.nodes.length).toBeGreaterThanOrEqual(4);
  });

  it("handles invalid node ID gracefully", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeIds: [printNodeId, "00000000-0000-0000-0000-000000000000"],
    });
    expect(data.error).toBeUndefined();
    expect(data.duplicatedCount).toBe(1);
    expect(data.notFound).toBeDefined();
    expect(data.notFound.length).toBeGreaterThan(0);
  });

  it("rejects empty nodeIds array", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeIds: [],
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: "BP_Nonexistent_XYZ_999",
      graph: "EventGraph",
      nodeIds: ["00000000-0000-0000-0000-000000000000"],
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent graph", async () => {
    const data = await uePost("/api/duplicate-nodes", {
      blueprint: bpName,
      graph: "NonExistentGraph_XYZ",
      nodeIds: [printNodeId],
    });
    expect(data.error).toBeDefined();
  });
});
