import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { ueGet, uePost, createTestAnimBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("animation blueprint read tools", () => {
  const bpName = uniqueName("ABP_ReadTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const result = await createTestAnimBlueprint({ name: bpName });
    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);
    expect(result.isAnimBlueprint).toBe(true);
  }, 60_000);

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  }, 30_000);

  it("get_blueprint returns isAnimBlueprint and targetSkeleton", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    expect(data.isAnimBlueprint).toBe(true);
    expect(data.targetSkeleton).toBeDefined();
  });

  it("get_blueprint_graph returns graphType for AnimGraph", async () => {
    const data = await ueGet("/api/graph", { name: bpName, graph: "AnimGraph" });
    expect(data.error).toBeUndefined();
    expect(data.graphType).toBe("AnimGraph");
  });

  it("returns error for non-existent blueprint", async () => {
    const data = await ueGet("/api/blueprint", { name: "ABP_NonexistentXYZ_999" });
    expect(data.error).toBeDefined();
  });
});
