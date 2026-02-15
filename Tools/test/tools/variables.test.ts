import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("add_variable / remove_variable", () => {
  const bpName = uniqueName("BP_VariablesTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // --- add_variable tests ---

  it("adds a bool variable", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "bIsActive",
      variableType: "bool",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("bIsActive");
    expect(data.variableType).toBe("bool");
    expect(data.saved).toBe(true);
  });

  it("adds a float variable with category", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "Health",
      variableType: "float",
      category: "Stats",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("Health");
    expect(data.category).toBe("Stats");
    expect(data.saved).toBe(true);
  });

  it("adds an int variable", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "Score",
      variableType: "int",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("Score");
    expect(data.saved).toBe(true);
  });

  it("adds a string variable", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "PlayerName",
      variableType: "string",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("PlayerName");
    expect(data.saved).toBe(true);
  });

  it("adds a vector variable (built-in struct)", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "SpawnLocation",
      variableType: "vector",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("SpawnLocation");
    expect(data.saved).toBe(true);
  });

  it("adds an array variable", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "Inventory",
      variableType: "string",
      isArray: true,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("Inventory");
    expect(data.isArray).toBe(true);
    expect(data.saved).toBe(true);
  });

  it("variable appears in get_blueprint response", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    const varNames = data.variables.map((v: any) => v.name);
    expect(varNames).toContain("bIsActive");
    expect(varNames).toContain("Health");
    expect(varNames).toContain("Score");
  });

  it("rejects duplicate variable name", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "bIsActive",
      variableType: "bool",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects unknown type", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
      variableName: "BadVar",
      variableType: "NonExistentType_XYZ",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects non-existent blueprint", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: "BP_Nonexistent_XYZ_999",
      variableName: "Foo",
      variableType: "bool",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing required fields", async () => {
    const data = await uePost("/api/add-variable", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  // --- VariableGet/VariableSet nodes for the new variable ---

  it("can add a VariableGet node for the new variable", async () => {
    const data = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "VariableGet",
      variableName: "Health",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.nodeId).toBeDefined();
  });

  it("can add a VariableSet node for the new variable", async () => {
    const data = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "VariableSet",
      variableName: "Health",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.nodeId).toBeDefined();
  });

  // --- remove_variable tests ---

  it("removes a variable", async () => {
    const data = await uePost("/api/remove-variable", {
      blueprint: bpName,
      variableName: "Score",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.variableName).toBe("Score");
    expect(data.saved).toBe(true);
  });

  it("removed variable is gone from get_blueprint", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    const varNames = data.variables.map((v: any) => v.name);
    expect(varNames).not.toContain("Score");
  });

  it("rejects removal of non-existent variable", async () => {
    const data = await uePost("/api/remove-variable", {
      blueprint: bpName,
      variableName: "NonExistentVar_XYZ",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
    expect(data.availableVariables).toBeDefined();
  });

  it("rejects removal with missing required fields", async () => {
    const data = await uePost("/api/remove-variable", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects removal from non-existent blueprint", async () => {
    const data = await uePost("/api/remove-variable", {
      blueprint: "BP_Nonexistent_XYZ_999",
      variableName: "Foo",
    });
    expect(data.error).toBeDefined();
  });
});
