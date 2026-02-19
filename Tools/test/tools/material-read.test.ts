import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestMaterial, deleteTestMaterial, uniqueName } from "../helpers.js";

describe("material read tools", () => {
  const matName = uniqueName("M_ReadTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const result = await createTestMaterial({ name: matName });
    expect(result.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestMaterial(`${packagePath}/${matName}`);
  });

  describe("list_materials", () => {
    it("returns materials list", async () => {
      const data = await ueGet("/api/materials", {});
      expect(data.error).toBeUndefined();
      expect(data.count).toBeGreaterThanOrEqual(1);
      expect(data.materials).toBeDefined();
      expect(Array.isArray(data.materials)).toBe(true);
    });

    it("filters by name", async () => {
      const data = await ueGet("/api/materials", { filter: matName });
      expect(data.error).toBeUndefined();
      expect(data.count).toBeGreaterThanOrEqual(1);
      const found = data.materials.some((m: any) => m.name === matName);
      expect(found).toBe(true);
    });

    it("filters by type", async () => {
      const data = await ueGet("/api/materials", { type: "material" });
      expect(data.error).toBeUndefined();
      for (const m of data.materials) {
        expect(m.type).toBe("Material");
      }
    });
  });

  describe("get_material", () => {
    it("returns material details", async () => {
      const data = await ueGet("/api/material", { name: matName });
      expect(data.error).toBeUndefined();
      expect(data.name).toBe(matName);
      expect(data.domain).toBeDefined();
      expect(data.blendMode).toBeDefined();
    });

    it("returns usage flags", async () => {
      const data = await ueGet("/api/material", { name: matName });
      expect(data.error).toBeUndefined();
      expect(data.usageFlags).toBeDefined();
      expect(typeof data.usageFlags.bUsedWithSkeletalMesh).toBe("boolean");
      expect(typeof data.usageFlags.bUsedWithMorphTargets).toBe("boolean");
      expect(typeof data.usageFlags.bUsedWithNiagaraSprites).toBe("boolean");
      expect(typeof data.usageFlags.bUsedWithParticleSprites).toBe("boolean");
      expect(typeof data.usageFlags.bUsedWithStaticLighting).toBe("boolean");
    });

    it("returns opacityMaskClipValue", async () => {
      const data = await ueGet("/api/material", { name: matName });
      expect(data.error).toBeUndefined();
      expect(typeof data.opacityMaskClipValue).toBe("number");
    });

    it("returns additional settings", async () => {
      const data = await ueGet("/api/material", { name: matName });
      expect(data.error).toBeUndefined();
      expect(typeof data.ditheredLODTransition).toBe("boolean");
      expect(typeof data.bAllowNegativeEmissiveColor).toBe("boolean");
    });

    it("returns textureSampleCount", async () => {
      const data = await ueGet("/api/material", { name: matName });
      expect(data.error).toBeUndefined();
      expect(typeof data.textureSampleCount).toBe("number");
    });

    it("returns error for non-existent material", async () => {
      const data = await ueGet("/api/material", { name: "M_NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });

    it("rejects missing name", async () => {
      const data = await ueGet("/api/material", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("get_material_graph", () => {
    it("returns graph data", async () => {
      const data = await ueGet("/api/material-graph", { name: matName });
      expect(data.error).toBeUndefined();
      expect(data.nodes).toBeDefined();
    });

    it("returns error for non-existent material", async () => {
      const data = await ueGet("/api/material-graph", { name: "M_NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });
  });

  describe("describe_material", () => {
    it("returns description", async () => {
      const data = await uePost("/api/describe-material", { material: matName });
      expect(data.error).toBeUndefined();
      expect(data.material).toBe(matName);
    });

    it("returns error for missing material field", async () => {
      const data = await uePost("/api/describe-material", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("search_materials", () => {
    it("returns results for query", async () => {
      const data = await ueGet("/api/search-materials", { query: matName });
      expect(data.error).toBeUndefined();
      expect(data.resultCount).toBeGreaterThanOrEqual(0);
      expect(data.results).toBeDefined();
    });
  });

  describe("find_material_references", () => {
    it("returns references data", async () => {
      const data = await uePost("/api/material-references", { material: matName });
      expect(data.error).toBeUndefined();
      expect(data.totalReferencers).toBeDefined();
    });

    it("returns error for missing material field", async () => {
      const data = await uePost("/api/material-references", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("list_material_functions", () => {
    it("returns function list", async () => {
      const data = await ueGet("/api/material-functions", {});
      expect(data.error).toBeUndefined();
      expect(data.count).toBeDefined();
      expect(data.functions).toBeDefined();
    });
  });
});
