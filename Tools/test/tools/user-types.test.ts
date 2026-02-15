import { describe, it, expect, afterAll } from "vitest";
import { uePost, uniqueName } from "../helpers.js";

describe("user_types", () => {
  const structPath = `/Game/Test/${uniqueName("S_TestStruct")}`;
  const enumPath = `/Game/Test/${uniqueName("E_TestEnum")}`;

  afterAll(async () => {
    // Clean up created assets
    await uePost("/api/delete-asset", { assetPath: structPath });
    await uePost("/api/delete-asset", { assetPath: enumPath });
  });

  // --- create_struct ---

  it("creates a struct with properties", async () => {
    const data = await uePost("/api/create-struct", {
      assetPath: structPath,
      properties: [
        { name: "Health", type: "float" },
        { name: "Name", type: "string" },
        { name: "IsAlive", type: "bool" },
      ],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.assetPath).toBe(structPath);
    expect(data.propertiesAdded).toBe(3);
    expect(data.saved).toBe(true);
  });

  it("rejects creating struct at existing path", async () => {
    const data = await uePost("/api/create-struct", {
      assetPath: structPath,
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing assetPath for struct", async () => {
    const data = await uePost("/api/create-struct", {});
    expect(data.error).toBeDefined();
  });

  // --- add_struct_property ---

  it("adds a property to existing struct", async () => {
    const data = await uePost("/api/add-struct-property", {
      assetPath: structPath,
      name: "Score",
      type: "int",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.propertyName).toBe("Score");
    expect(data.saved).toBe(true);
  });

  it("rejects adding to non-existent struct", async () => {
    const data = await uePost("/api/add-struct-property", {
      assetPath: "/Game/Test/S_Nonexistent_XYZ_999",
      name: "Foo",
      type: "bool",
    });
    expect(data.error).toBeDefined();
  });

  // --- remove_struct_property ---

  it("removes a property from struct", async () => {
    const data = await uePost("/api/remove-struct-property", {
      assetPath: structPath,
      name: "Score",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.removedProperty).toBe("Score");
    expect(data.saved).toBe(true);
  });

  it("returns error for non-existent property", async () => {
    const data = await uePost("/api/remove-struct-property", {
      assetPath: structPath,
      name: "FakeProperty_XYZ",
    });
    expect(data.error).toBeDefined();
    expect(data.availableProperties).toBeDefined();
  });

  // --- create_enum ---

  it("creates an enum with values", async () => {
    const data = await uePost("/api/create-enum", {
      assetPath: enumPath,
      values: ["Low", "Medium", "High", "Critical"],
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.assetPath).toBe(enumPath);
    expect(data.valueCount).toBe(4);
    expect(data.saved).toBe(true);
  });

  it("rejects enum with no values", async () => {
    const data = await uePost("/api/create-enum", {
      assetPath: "/Game/Test/E_Empty",
      values: [],
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing assetPath for enum", async () => {
    const data = await uePost("/api/create-enum", { values: ["A"] });
    expect(data.error).toBeDefined();
  });
});
