import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("add_event_dispatcher / list_event_dispatchers / add_function_parameter", () => {
  const bpName = uniqueName("BP_EventDispTest");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  // --- list_event_dispatchers tests (empty) ---

  it("lists dispatchers on a BP with none (empty)", async () => {
    const data = await uePost("/api/list-event-dispatchers", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.blueprint).toBe(bpName);
    expect(data.count).toBe(0);
    expect(data.dispatchers).toEqual([]);
  });

  // --- add_event_dispatcher tests ---

  it("adds an event dispatcher with no parameters", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: bpName,
      dispatcherName: "OnSimpleEvent",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.blueprint).toBe(bpName);
    expect(data.dispatcherName).toBe("OnSimpleEvent");
    expect(data.parameters).toEqual([]);
    expect(data.saved).toBe(true);
  });

  it("adds an event dispatcher with parameters", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: bpName,
      dispatcherName: "OnDamageTaken",
      parameters: [
        { name: "Damage", type: "float" },
        { name: "Instigator", type: "object" },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.dispatcherName).toBe("OnDamageTaken");
    expect(data.parameters).toHaveLength(2);
    expect(data.parameters[0].name).toBe("Damage");
    expect(data.parameters[0].type).toBe("float");
    expect(data.parameters[1].name).toBe("Instigator");
    expect(data.parameters[1].type).toBe("object");
    expect(data.saved).toBe(true);
  });

  it("lists dispatchers after adding two", async () => {
    const data = await uePost("/api/list-event-dispatchers", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.count).toBe(2);
    expect(data.dispatchers).toHaveLength(2);

    const names = data.dispatchers.map((d: any) => d.name);
    expect(names).toContain("OnSimpleEvent");
    expect(names).toContain("OnDamageTaken");

    // Check that OnDamageTaken has parameters
    const damageTaken = data.dispatchers.find((d: any) => d.name === "OnDamageTaken");
    expect(damageTaken.parameters).toHaveLength(2);
    expect(damageTaken.parameters[0].name).toBe("Damage");
    expect(damageTaken.parameters[1].name).toBe("Instigator");
  });

  it("rejects adding duplicate dispatcher", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: bpName,
      dispatcherName: "OnSimpleEvent",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects adding dispatcher to non-existent blueprint", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: "BP_Nonexistent_XYZ_999",
      dispatcherName: "OnFoo",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects adding dispatcher with missing required fields", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects dispatcher with invalid parameter type", async () => {
    const data = await uePost("/api/add-event-dispatcher", {
      blueprint: bpName,
      dispatcherName: "OnBadType",
      parameters: [{ name: "BadParam", type: "NonExistentType_XYZ" }],
    });
    expect(data.error).toBeDefined();
  });

  // --- add_function_parameter tests ---

  it("adds a parameter to a function", async () => {
    // First create a function graph
    const createRes = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "TestFunction",
      graphType: "function",
    });
    expect(createRes.error).toBeUndefined();

    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "TestFunction",
      paramName: "InputValue",
      paramType: "float",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.blueprint).toBe(bpName);
    expect(data.functionName).toBe("TestFunction");
    expect(data.paramName).toBe("InputValue");
    expect(data.paramType).toBe("float");
    expect(data.nodeType).toBe("FunctionEntry");
    expect(data.saved).toBe(true);
  });

  it("adds a parameter to a custom event", async () => {
    // Create a custom event
    const createRes = await uePost("/api/create-graph", {
      blueprint: bpName,
      graphName: "TestCustomEvent",
      graphType: "customEvent",
    });
    expect(createRes.error).toBeUndefined();

    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "TestCustomEvent",
      paramName: "EventData",
      paramType: "string",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.functionName).toBe("TestCustomEvent");
    expect(data.paramName).toBe("EventData");
    expect(data.nodeType).toBe("CustomEvent");
    expect(data.saved).toBe(true);
  });

  it("adds a parameter to an event dispatcher", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "OnSimpleEvent",
      paramName: "NewParam",
      paramType: "bool",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.functionName).toBe("OnSimpleEvent");
    expect(data.paramName).toBe("NewParam");
    expect(data.nodeType).toBe("EventDispatcher");
    expect(data.saved).toBe(true);
  });

  it("verifies dispatcher parameter was added via list", async () => {
    const data = await uePost("/api/list-event-dispatchers", { blueprint: bpName });
    expect(data.error).toBeUndefined();

    const simple = data.dispatchers.find((d: any) => d.name === "OnSimpleEvent");
    expect(simple).toBeDefined();
    expect(simple.parameters).toHaveLength(1);
    expect(simple.parameters[0].name).toBe("NewParam");
  });

  it("rejects duplicate parameter name", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "TestFunction",
      paramName: "InputValue",
      paramType: "int",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already exists");
  });

  it("rejects adding parameter to non-existent function", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "NonExistentFunc_XYZ",
      paramName: "Foo",
      paramType: "bool",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
    expect(data.availableFunctions).toBeDefined();
    expect(data.availableFunctions.length).toBeGreaterThan(0);
  });

  it("rejects adding parameter to non-existent blueprint", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: "BP_Nonexistent_XYZ_999",
      functionName: "SomeFunc",
      paramName: "Foo",
      paramType: "bool",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects adding parameter with missing required fields", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "TestFunction",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects adding parameter with unknown type", async () => {
    const data = await uePost("/api/add-function-parameter", {
      blueprint: bpName,
      functionName: "TestFunction",
      paramName: "BadTypeParam",
      paramType: "NonExistentType_XYZ",
    });
    expect(data.error).toBeDefined();
  });

  // --- list error cases ---

  it("rejects listing dispatchers on non-existent blueprint", async () => {
    const data = await uePost("/api/list-event-dispatchers", {
      blueprint: "BP_Nonexistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects listing dispatchers with missing required fields", async () => {
    const data = await uePost("/api/list-event-dispatchers", {});
    expect(data.error).toBeDefined();
  });
});
