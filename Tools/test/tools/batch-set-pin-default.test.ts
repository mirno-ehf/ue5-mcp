import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("batch set_pin_default", () => {
  const bpName = uniqueName("BP_BatchPinTest");
  const packagePath = "/Game/Test";
  let printNodeId1: string;
  let printNodeId2: string;

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Add two PrintString nodes
    const n1 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(n1.success).toBe(true);
    printNodeId1 = n1.nodeId;

    const n2 = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(n2.success).toBe(true);
    printNodeId2 = n2.nodeId;
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // Verify single mode still works
  it("sets a single pin default (backwards compatible)", async () => {
    const data = await uePost("/api/set-pin-default", {
      blueprint: bpName,
      nodeId: printNodeId1,
      pinName: "InString",
      value: "Hello World",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.newValue).toBe("Hello World");
    expect(data.saved).toBe(true);
  });

  it("sets multiple pin defaults in batch", async () => {
    const data = await uePost("/api/set-pin-default", {
      batch: [
        { blueprint: bpName, nodeId: printNodeId1, pinName: "InString", value: "Batch Value 1" },
        { blueprint: bpName, nodeId: printNodeId2, pinName: "InString", value: "Batch Value 2" },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.successCount).toBe(2);
    expect(data.totalCount).toBe(2);
    expect(data.results).toHaveLength(2);
    expect(data.results[0].success).toBe(true);
    expect(data.results[1].success).toBe(true);
    expect(data.saved).toBe(true);
  });

  it("handles errors in batch gracefully", async () => {
    const data = await uePost("/api/set-pin-default", {
      batch: [
        { blueprint: bpName, nodeId: printNodeId1, pinName: "InString", value: "Good" },
        { blueprint: bpName, nodeId: "00000000-0000-0000-0000-000000000000", pinName: "InString", value: "Bad" },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.successCount).toBe(1);
    expect(data.totalCount).toBe(2);
    expect(data.results[0].success).toBe(true);
    expect(data.results[1].error).toBeDefined();
  });

  it("handles empty batch gracefully", async () => {
    // Empty batch should fall through to single mode which will error on missing fields
    const data = await uePost("/api/set-pin-default", {
      batch: [],
    });
    // With empty batch, it should fall through to single mode error
    expect(data.error).toBeDefined();
  });

  it("verifies batch values persist", async () => {
    // First set values in batch
    await uePost("/api/set-pin-default", {
      batch: [
        { blueprint: bpName, nodeId: printNodeId1, pinName: "InString", value: "Persisted1" },
        { blueprint: bpName, nodeId: printNodeId2, pinName: "InString", value: "Persisted2" },
      ],
    });

    // Verify by reading the graph
    const graph = await ueGet("/api/graph", { name: bpName, graph: "EventGraph" });
    expect(graph.error).toBeUndefined();

    const node1 = graph.nodes.find((n: any) => n.id === printNodeId1);
    const node2 = graph.nodes.find((n: any) => n.id === printNodeId2);
    expect(node1).toBeDefined();
    expect(node2).toBeDefined();

    const pin1 = node1.pins?.find((p: any) => p.name === "InString");
    const pin2 = node2.pins?.find((p: any) => p.name === "InString");
    if (pin1) expect(pin1.default).toBe("Persisted1");
    if (pin2) expect(pin2.default).toBe("Persisted2");
  });
});
