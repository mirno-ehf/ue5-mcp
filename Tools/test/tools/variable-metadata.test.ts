import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("set_variable_metadata", () => {
  const bpName = uniqueName("BP_VarMetadataTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Add test variables
    const v1 = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "Health",
      variableType: "float",
    });
    expect(v1.error).toBeUndefined();

    const v2 = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "bIsAlive",
      variableType: "bool",
    });
    expect(v2.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("sets category on a variable", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      category: "Stats",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.changes).toHaveLength(1);
    expect(data.changes[0].field).toBe("category");
    expect(data.changes[0].newValue).toBe("Stats");
    expect(data.saved).toBe(true);
  });

  it("sets tooltip on a variable", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      tooltip: "Current health points",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.changes).toHaveLength(1);
    expect(data.changes[0].field).toBe("tooltip");
    expect(data.saved).toBe(true);
  });

  it("sets multiple fields at once", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "bIsAlive",
      category: "State",
      tooltip: "Whether the character is alive",
      editability: "editAnywhere",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.changes.length).toBeGreaterThanOrEqual(3);
    expect(data.saved).toBe(true);
  });

  it("sets replication to replicated", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      replication: "replicated",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    const repChange = data.changes.find((c: any) => c.field === "replication");
    expect(repChange).toBeDefined();
    expect(repChange.newValue).toBe("replicated");
  });

  it("sets exposeOnSpawn", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      exposeOnSpawn: true,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
  });

  it("sets editability to editDefaultsOnly", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      editability: "editDefaultsOnly",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
  });

  it("rejects invalid replication value", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      replication: "invalid_value",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects invalid editability value", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
      editability: "invalid_value",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects no metadata fields provided", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "Health",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent variable", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
      variable: "NonExistentVar_XYZ",
      category: "Test",
    });
    expect(data.error).toBeDefined();
    expect(data.availableVariables).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: "BP_Nonexistent_XYZ_999",
      variable: "Health",
      category: "Test",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/set-variable-metadata", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });
});
