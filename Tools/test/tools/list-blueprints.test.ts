import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("list_blueprints", () => {
  const bpName = uniqueName("BP_ListTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("returns count and blueprints array", async () => {
    const data = await ueGet("/api/list");
    expect(data.error).toBeUndefined();
    expect(typeof data.count).toBe("number");
    expect(typeof data.total).toBe("number");
    expect(Array.isArray(data.blueprints)).toBe(true);
    expect(data.count).toBeGreaterThan(0);
  });

  it("filters by name substring", async () => {
    const data = await ueGet("/api/list", { filter: bpName });
    expect(data.count).toBe(1);
    expect(data.blueprints[0].name).toBe(bpName);
  });

  it("returns empty results for non-matching filter", async () => {
    const data = await ueGet("/api/list", { filter: "ZZZ_NonExistent_XYZ" });
    expect(data.count).toBe(0);
    expect(data.blueprints).toHaveLength(0);
  });

  it("blueprint entries have expected fields", async () => {
    const data = await ueGet("/api/list", { filter: bpName });
    const bp = data.blueprints[0];
    expect(bp.name).toBeDefined();
    expect(bp.path).toBeDefined();
  });

  it("type=regular excludes level blueprints", async () => {
    const data = await ueGet("/api/list", { type: "regular" });
    expect(data.error).toBeUndefined();
    const levelBPs = data.blueprints.filter((bp: any) => bp.isLevelBlueprint);
    expect(levelBPs).toHaveLength(0);
  });

  it("type=level returns only level blueprints", async () => {
    const data = await ueGet("/api/list", { type: "level" });
    expect(data.error).toBeUndefined();
    // Every entry should be a level blueprint
    for (const bp of data.blueprints) {
      expect(bp.isLevelBlueprint).toBe(true);
    }
  });

  it("type=all returns both regular and level blueprints", async () => {
    const all = await ueGet("/api/list", { type: "all" });
    const regular = await ueGet("/api/list", { type: "regular" });
    const level = await ueGet("/api/list", { type: "level" });
    expect(all.error).toBeUndefined();
    expect(all.count).toBe(regular.count + level.count);
  });

  it("regular blueprint entries do not have isLevelBlueprint", async () => {
    const data = await ueGet("/api/list", { filter: bpName });
    expect(data.count).toBe(1);
    expect(data.blueprints[0].isLevelBlueprint).toBeUndefined();
  });
});
