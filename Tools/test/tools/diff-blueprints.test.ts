import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("diff_blueprints", () => {
  const bpNameA = uniqueName("BP_DiffTestA");
  const bpNameB = uniqueName("BP_DiffTestB");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const a = await createTestBlueprint({ name: bpNameA });
    expect(a.error).toBeUndefined();
    const b = await createTestBlueprint({ name: bpNameB });
    expect(b.error).toBeUndefined();

    // Add a node to A but not B to create a difference
    const node = await uePost("/api/add-node", {
      blueprint: bpNameA,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(node.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpNameA}`);
    await deleteTestBlueprint(`${packagePath}/${bpNameB}`);
  });

  it("diffs two blueprints and finds differences", async () => {
    const data = await uePost("/api/diff-blueprints", {
      blueprintA: bpNameA,
      blueprintB: bpNameB,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.blueprintA).toBe(bpNameA);
    expect(data.blueprintB).toBe(bpNameB);
    expect(data.graphs).toBeDefined();
    expect(data.totalDifferences).toBeGreaterThan(0);
  });

  it("diffs identical blueprints", async () => {
    const data = await uePost("/api/diff-blueprints", {
      blueprintA: bpNameB,
      blueprintB: bpNameB,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    // Same BP compared to itself should be identical
    for (const g of data.graphs) {
      expect(g.status).toBe("identical");
    }
  });

  it("filters by graph name", async () => {
    const data = await uePost("/api/diff-blueprints", {
      blueprintA: bpNameA,
      blueprintB: bpNameB,
      graph: "EventGraph",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.graphs.length).toBe(1);
    expect(data.graphs[0].graph).toBe("EventGraph");
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/diff-blueprints", {
      blueprintA: bpNameA,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/diff-blueprints", {
      blueprintA: bpNameA,
      blueprintB: "BP_Nonexistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
  });
});
