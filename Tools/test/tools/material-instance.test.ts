import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestMaterial, createTestMaterialInstance, deleteTestMaterial, uniqueName } from "../helpers.js";

describe("material instance tools", () => {
  const parentName = uniqueName("M_MIParent");
  const miName = uniqueName("MI_Test");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    // Create parent material first
    const parentResult = await createTestMaterial({ name: parentName });
    expect(parentResult.error).toBeUndefined();

    // Add a scalar parameter to the parent so we can test instance overrides
    await uePost("/api/add-material-expression", {
      material: parentName,
      expressionClass: "ScalarParameter",
      posX: -200,
      posY: 0,
    });
  });

  afterAll(async () => {
    await deleteTestMaterial(`${packagePath}/${miName}`);
    await deleteTestMaterial(`${packagePath}/${parentName}`);
  });

  describe("create_material_instance", () => {
    it("creates a material instance", async () => {
      const data = await uePost("/api/create-material-instance", {
        name: miName,
        packagePath,
        parentMaterial: parentName,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.name).toBe(miName);
    });

    it("rejects missing parent", async () => {
      const data = await uePost("/api/create-material-instance", {
        name: "MI_NoParent",
        packagePath,
        parentMaterial: "M_NonExistent_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("get_material_instance_parameters", () => {
    it("returns parameter info", async () => {
      const data = await ueGet("/api/material-instance-params", { name: miName });
      expect(data.error).toBeUndefined();
      expect(data.name).toBe(miName);
      expect(data.parentChain).toBeDefined();
    });

    it("returns error for non-existent MI", async () => {
      const data = await ueGet("/api/material-instance-params", { name: "MI_NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });
  });

  describe("reparent_material_instance", () => {
    const newParentName = uniqueName("M_NewParent");

    beforeAll(async () => {
      await createTestMaterial({ name: newParentName });
    });

    afterAll(async () => {
      await deleteTestMaterial(`${packagePath}/${newParentName}`);
    });

    it("changes the parent material", async () => {
      const data = await uePost("/api/reparent-material-instance", {
        materialInstance: miName,
        newParent: newParentName,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
    });
  });
});
