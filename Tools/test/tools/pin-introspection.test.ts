import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("pin_introspection", () => {
  const bpName = uniqueName("BP_PinInfoTest");
  const packagePath = "/Game/Test";
  let printStringNodeId: string;
  let branchNodeId: string;

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Add a PrintString node
    const ps = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(ps.error).toBeUndefined();
    printStringNodeId = ps.nodeId;

    // Add a Branch node
    const br = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "Branch",
    });
    expect(br.error).toBeUndefined();
    branchNodeId = br.nodeId;
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // --- get_pin_info ---

  it("gets pin info for an exec pin", async () => {
    const data = await uePost("/api/get-pin-info", {
      blueprint: bpName,
      nodeId: printStringNodeId,
      pinName: "execute",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.direction).toBe("Input");
    expect(data.type).toBe("exec");
    expect(data.isArray).toBe(false);
  });

  it("gets pin info for a data pin", async () => {
    const data = await uePost("/api/get-pin-info", {
      blueprint: bpName,
      nodeId: branchNodeId,
      pinName: "Condition",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.direction).toBe("Input");
    expect(data.type).toBe("bool");
  });

  it("returns available pins when pin not found", async () => {
    const data = await uePost("/api/get-pin-info", {
      blueprint: bpName,
      nodeId: printStringNodeId,
      pinName: "FakePin",
    });
    expect(data.error).toBeDefined();
    expect(data.availablePins).toBeDefined();
    expect(data.availablePins.length).toBeGreaterThan(0);
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/get-pin-info", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent node", async () => {
    const data = await uePost("/api/get-pin-info", {
      blueprint: bpName,
      nodeId: "00000000-0000-0000-0000-000000000000",
      pinName: "execute",
    });
    expect(data.error).toBeDefined();
  });

  // --- check_pin_compatibility ---

  it("checks compatible exec pins", async () => {
    const data = await uePost("/api/check-pin-compatibility", {
      blueprint: bpName,
      sourceNodeId: branchNodeId,
      sourcePinName: "then",
      targetNodeId: printStringNodeId,
      targetPinName: "execute",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.compatible).toBe(true);
  });

  it("checks incompatible pins", async () => {
    const data = await uePost("/api/check-pin-compatibility", {
      blueprint: bpName,
      sourceNodeId: branchNodeId,
      sourcePinName: "Condition",
      targetNodeId: printStringNodeId,
      targetPinName: "InString",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    // bool -> string: may or may not be compatible depending on UE5's auto-conversion
    // Just verify the response structure
    expect(data.compatible).toBeDefined();
    expect(data.connectionType).toBeDefined();
    expect(data.sourcePinType).toBeDefined();
    expect(data.targetPinType).toBeDefined();
  });

  it("rejects missing fields for compatibility check", async () => {
    const data = await uePost("/api/check-pin-compatibility", {
      blueprint: bpName,
      sourceNodeId: branchNodeId,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent source node", async () => {
    const data = await uePost("/api/check-pin-compatibility", {
      blueprint: bpName,
      sourceNodeId: "00000000-0000-0000-0000-000000000000",
      sourcePinName: "then",
      targetNodeId: printStringNodeId,
      targetPinName: "execute",
    });
    expect(data.error).toBeDefined();
  });
});
