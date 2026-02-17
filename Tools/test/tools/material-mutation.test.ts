import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestMaterial, deleteTestMaterial, uniqueName } from "../helpers.js";

describe("material mutation tools", () => {
  const matName = uniqueName("M_MutTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const result = await createTestMaterial({ name: matName });
    expect(result.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestMaterial(`${packagePath}/${matName}`);
  });

  describe("create_material", () => {
    const createName = uniqueName("M_CreateTest");

    afterAll(async () => {
      await deleteTestMaterial(`/Game/Test/${createName}`);
    });

    it("creates a new material", async () => {
      const data = await uePost("/api/create-material", {
        name: createName,
        packagePath: "/Game/Test",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.name).toBe(createName);
    });

    it("rejects missing name", async () => {
      const data = await uePost("/api/create-material", { packagePath: "/Game/Test" });
      expect(data.error).toBeDefined();
    });
  });

  describe("add_material_expression", () => {
    it("adds a constant expression", async () => {
      const data = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "Constant",
        posX: 100,
        posY: 100,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.nodeId).toBeDefined();
    });

    it("adds a scalar parameter", async () => {
      const data = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "ScalarParameter",
        posX: -200,
        posY: 0,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
    });

    it("rejects invalid expression class", async () => {
      const data = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "InvalidExpressionClass",
      });
      expect(data.error).toBeDefined();
    });

    it("supports dry run", async () => {
      const data = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "Constant",
        dryRun: true,
      });
      expect(data.error).toBeUndefined();
      expect(data.dryRun).toBe(true);
    });
  });

  describe("set_expression_value", () => {
    let constantNodeId: string;

    beforeAll(async () => {
      const result = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "Constant",
        posX: 300,
        posY: 300,
      });
      constantNodeId = result.nodeId;
    });

    it("sets constant value", async () => {
      const data = await uePost("/api/set-expression-value", {
        material: matName,
        nodeId: constantNodeId,
        value: 0.75,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
    });
  });

  describe("move_material_expression", () => {
    let nodeId: string;

    beforeAll(async () => {
      const result = await uePost("/api/add-material-expression", {
        material: matName,
        expressionClass: "Constant",
        posX: 0,
        posY: 0,
      });
      nodeId = result.nodeId;
    });

    it("moves expression to new position", async () => {
      const data = await uePost("/api/move-material-expression", {
        material: matName,
        nodeId,
        posX: 500,
        posY: 250,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
    });
  });

  describe("snapshot and diff", () => {
    let snapshotId: string;

    it("takes a snapshot", async () => {
      const data = await uePost("/api/snapshot-material-graph", {
        material: matName,
      });
      expect(data.error).toBeUndefined();
      expect(data.snapshotId).toBeDefined();
      snapshotId = data.snapshotId;
    });

    it("diffs against snapshot", async () => {
      const data = await uePost("/api/diff-material-graph", {
        material: matName,
        snapshotId,
      });
      expect(data.error).toBeUndefined();
      expect(data.summary).toBeDefined();
    });

    it("returns error for invalid snapshot", async () => {
      const data = await uePost("/api/diff-material-graph", {
        material: matName,
        snapshotId: "nonexistent_snapshot_id",
      });
      expect(data.error).toBeDefined();
    });
  });
});
