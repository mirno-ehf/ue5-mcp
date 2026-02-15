import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("move_node", () => {
  const bpName = uniqueName("BP_MoveNodeTest");
  const packagePath = "/Game/Test";
  let nodeId1: string;
  let nodeId2: string;

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Add two nodes to reposition
    const n1 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
      posX: 0,
      posY: 0,
    });
    expect(n1.success).toBe(true);
    nodeId1 = n1.nodeId;

    const n2 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "Branch",
      posX: 100,
      posY: 100,
    });
    expect(n2.success).toBe(true);
    nodeId2 = n2.nodeId;
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("moves a single node", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      nodeId: nodeId1,
      x: 500,
      y: 300,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.nodeId).toBe(nodeId1);
    expect(data.newX).toBe(500);
    expect(data.newY).toBe(300);
    expect(data.saved).toBe(true);
  });

  it("verifies node position in graph", async () => {
    const graph = await ueGet("/api/graph", { name: bpName, graph: "EventGraph" });
    expect(graph.error).toBeUndefined();
    const node = graph.nodes.find((n: any) => n.id === nodeId1);
    expect(node).toBeDefined();
    expect(node.posX).toBe(500);
    expect(node.posY).toBe(300);
  });

  it("moves multiple nodes in batch", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      nodes: [
        { nodeId: nodeId1, x: 100, y: 200 },
        { nodeId: nodeId2, x: 400, y: 500 },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.movedCount).toBe(2);
    expect(data.results).toHaveLength(2);
    expect(data.saved).toBe(true);
  });

  it("handles invalid node in batch gracefully", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      nodes: [
        { nodeId: nodeId1, x: 0, y: 0 },
        { nodeId: "00000000-0000-0000-0000-000000000000", x: 100, y: 100 },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.movedCount).toBe(1);
    expect(data.results[1].error).toBeDefined();
  });

  it("rejects missing nodeId in single mode", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      x: 100,
      y: 100,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing coordinates in single mode", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      nodeId: nodeId1,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: "BP_Nonexistent_XYZ_999",
      nodeId: "00000000-0000-0000-0000-000000000000",
      x: 0,
      y: 0,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent node", async () => {
    const data = await uePost("/api/move-node", {
      blueprint: bpName,
      nodeId: "00000000-0000-0000-0000-000000000000",
      x: 0,
      y: 0,
    });
    expect(data.error).toBeDefined();
  });
});
