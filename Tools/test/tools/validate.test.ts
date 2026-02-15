import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("validate_blueprint", () => {
  const bpName = uniqueName("BP_ValidateTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const bp = await createTestBlueprint({ name: bpName });
    expect(bp.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("validates a clean Blueprint successfully", async () => {
    const data = await uePost("/api/validate-blueprint", {
      blueprint: bpName,
    });
    expect(data.error).toBeUndefined();
    expect(data.blueprint).toBe(bpName);
    expect(data.isValid).toBe(true);
  });

  it("returns error for non-existent blueprint", async () => {
    const data = await uePost("/api/validate-blueprint", {
      blueprint: "BP_DoesNotExist_XYZ_999",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/validate-blueprint", {});
    expect(data.error).toBeDefined();
  });
});

describe("validate_all_blueprints", () => {
  it("validates all blueprints in /Game/Test path", async () => {
    const data = await uePost("/api/validate-all-blueprints", {
      filter: "/Game/Test",
    });
    expect(data.error).toBeUndefined();
    expect(typeof data.totalChecked).toBe("number");
    expect(typeof data.totalPassed).toBe("number");
    expect(typeof data.totalFailed).toBe("number");
    expect(typeof data.totalMatching).toBe("number");
  });

  it("validates with no filter (may take longer)", async () => {
    const data = await uePost("/api/validate-all-blueprints", {});
    expect(data.error).toBeUndefined();
    expect(data.totalChecked).toBeGreaterThanOrEqual(0);
    expect(typeof data.totalMatching).toBe("number");
  });
});

describe("validate_all_blueprints pagination", () => {
  const bpNames = [
    uniqueName("BP_ValidatePag_A"),
    uniqueName("BP_ValidatePag_B"),
    uniqueName("BP_ValidatePag_C"),
  ];
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    for (const name of bpNames) {
      const bp = await createTestBlueprint({ name });
      expect(bp.error).toBeUndefined();
    }
  });

  afterAll(async () => {
    for (const name of bpNames) {
      await deleteTestBlueprint(`${packagePath}/${name}`);
    }
  });

  it("countOnly returns totalMatching without validation fields", async () => {
    const data = await uePost("/api/validate-all-blueprints", {
      filter: "BP_ValidatePag_",
      countOnly: true,
    });
    expect(data.error).toBeUndefined();
    expect(data.totalMatching).toBeGreaterThanOrEqual(3);
    // countOnly should NOT include validation result fields
    expect(data.totalChecked).toBeUndefined();
    expect(data.totalPassed).toBeUndefined();
    expect(data.totalFailed).toBeUndefined();
    expect(data.failed).toBeUndefined();
  });

  it("countOnly with non-matching filter returns 0", async () => {
    const data = await uePost("/api/validate-all-blueprints", {
      filter: "BP_NoSuchBlueprint_ZZZ_999",
      countOnly: true,
    });
    expect(data.error).toBeUndefined();
    expect(data.totalMatching).toBe(0);
  });

  it("offset + limit respects limit on totalChecked", async () => {
    const data = await uePost("/api/validate-all-blueprints", {
      filter: "BP_ValidatePag_",
      offset: 0,
      limit: 2,
    });
    expect(data.error).toBeUndefined();
    expect(data.totalMatching).toBeGreaterThanOrEqual(3);
    expect(data.totalChecked).toBeLessThanOrEqual(2);
  });

  it("offset beyond total returns totalChecked: 0", async () => {
    const data = await uePost("/api/validate-all-blueprints", {
      filter: "BP_ValidatePag_",
      offset: 9999,
      limit: 10,
    });
    expect(data.error).toBeUndefined();
    expect(data.totalMatching).toBeGreaterThanOrEqual(3);
    expect(data.totalChecked).toBe(0);
  });

  it("offset=0, limit=0 matches unparameterized call (backward compat)", async () => {
    const withParams = await uePost("/api/validate-all-blueprints", {
      filter: "BP_ValidatePag_",
      offset: 0,
      limit: 0,
    });
    const withoutParams = await uePost("/api/validate-all-blueprints", {
      filter: "BP_ValidatePag_",
    });
    expect(withParams.error).toBeUndefined();
    expect(withoutParams.error).toBeUndefined();
    expect(withParams.totalChecked).toBe(withoutParams.totalChecked);
    expect(withParams.totalPassed).toBe(withoutParams.totalPassed);
    expect(withParams.totalFailed).toBe(withoutParams.totalFailed);
  });
});
