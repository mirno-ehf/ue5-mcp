import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("add_interface / remove_interface / list_interfaces", () => {
  const bpName = uniqueName("BP_InterfacesTest");
  const ifaceName = uniqueName("BPI_TestIface");
  const packagePath = "/Game/Test";

  beforeAll(async () => {
    // Create a regular Actor blueprint
    const bpRes = await createTestBlueprint({ name: bpName });
    expect(bpRes.error).toBeUndefined();

    // Create a Blueprint Interface asset
    const ifaceRes = await createTestBlueprint({
      name: ifaceName,
      blueprintType: "Interface",
    });
    expect(ifaceRes.error).toBeUndefined();
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
    await deleteTestBlueprint(`${packagePath}/${ifaceName}`);
  });

  // --- list_interfaces tests ---

  it("lists interfaces on a BP with none implemented (empty)", async () => {
    const data = await uePost("/api/list-interfaces", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.blueprint).toBe(bpName);
    expect(data.count).toBe(0);
    expect(data.interfaces).toEqual([]);
  });

  // --- add_interface tests ---

  it("adds an interface to a blueprint", async () => {
    const data = await uePost("/api/add-interface", {
      blueprint: bpName,
      interfaceName: ifaceName,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.blueprint).toBe(bpName);
    expect(data.interfaceName).toBeDefined();
    expect(data.interfacePath).toBeDefined();
    expect(data.functionGraphsAdded).toBeDefined();
    expect(data.saved).toBe(true);
  });

  it("lists interfaces after adding one", async () => {
    const data = await uePost("/api/list-interfaces", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.count).toBe(1);
    expect(data.interfaces).toHaveLength(1);
    expect(data.interfaces[0].name).toBeDefined();
    expect(data.interfaces[0].classPath).toBeDefined();
    expect(data.interfaces[0].functions).toBeDefined();
  });

  it("interface appears in get_blueprint response", async () => {
    const data = await ueGet("/api/blueprint", { name: bpName });
    expect(data.error).toBeUndefined();
    expect(data.interfaces).toBeDefined();
    expect(data.interfaces.length).toBeGreaterThanOrEqual(1);
  });

  it("rejects adding duplicate interface", async () => {
    const data = await uePost("/api/add-interface", {
      blueprint: bpName,
      interfaceName: ifaceName,
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("already implemented");
  });

  it("rejects adding non-existent interface", async () => {
    const data = await uePost("/api/add-interface", {
      blueprint: bpName,
      interfaceName: "BPI_Nonexistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not found");
  });

  it("rejects adding interface to non-existent blueprint", async () => {
    const data = await uePost("/api/add-interface", {
      blueprint: "BP_Nonexistent_XYZ_999",
      interfaceName: ifaceName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects add with missing required fields", async () => {
    const data = await uePost("/api/add-interface", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  // --- remove_interface tests ---

  it("removes an interface from a blueprint", async () => {
    const data = await uePost("/api/remove-interface", {
      blueprint: bpName,
      interfaceName: ifaceName,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.blueprint).toBe(bpName);
    expect(data.interfaceName).toBeDefined();
    expect(data.preservedFunctions).toBe(false);
    expect(data.saved).toBe(true);
  });

  it("lists interfaces after removal (empty again)", async () => {
    const data = await uePost("/api/list-interfaces", { blueprint: bpName });
    expect(data.error).toBeUndefined();
    expect(data.count).toBe(0);
    expect(data.interfaces).toEqual([]);
  });

  it("rejects removing non-implemented interface", async () => {
    const data = await uePost("/api/remove-interface", {
      blueprint: bpName,
      interfaceName: "BPI_NotImplemented_XYZ_999",
    });
    expect(data.error).toBeDefined();
    expect(data.error).toContain("not implemented");
    expect(data.implementedInterfaces).toBeDefined();
  });

  it("rejects removing interface from non-existent blueprint", async () => {
    const data = await uePost("/api/remove-interface", {
      blueprint: "BP_Nonexistent_XYZ_999",
      interfaceName: ifaceName,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects remove with missing required fields", async () => {
    const data = await uePost("/api/remove-interface", {
      blueprint: bpName,
    });
    expect(data.error).toBeDefined();
  });

  // --- list_interfaces error cases ---

  it("rejects list on non-existent blueprint", async () => {
    const data = await uePost("/api/list-interfaces", {
      blueprint: "BP_Nonexistent_XYZ_999",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects list with missing required fields", async () => {
    const data = await uePost("/api/list-interfaces", {});
    expect(data.error).toBeDefined();
  });

  // --- preserveFunctions option ---

  it("add and remove with preserveFunctions=true", async () => {
    // Re-add the interface
    const addData = await uePost("/api/add-interface", {
      blueprint: bpName,
      interfaceName: ifaceName,
    });
    expect(addData.error).toBeUndefined();
    expect(addData.success).toBe(true);

    // Remove with preserveFunctions=true
    const removeData = await uePost("/api/remove-interface", {
      blueprint: bpName,
      interfaceName: ifaceName,
      preserveFunctions: true,
    });
    expect(removeData.error).toBeUndefined();
    expect(removeData.success).toBe(true);
    expect(removeData.preservedFunctions).toBe(true);
    expect(removeData.saved).toBe(true);
  });
});
