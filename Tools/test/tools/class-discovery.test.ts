import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("class_discovery", () => {
  // --- list_classes ---

  it("lists classes with no filter", async () => {
    const data = await uePost("/api/list-classes", { limit: 10 });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.count).toBeGreaterThan(0);
    expect(data.classes).toBeDefined();
    expect(data.classes.length).toBeGreaterThan(0);
    // Verify structure
    const cls = data.classes[0];
    expect(cls.name).toBeDefined();
    expect(cls.isBlueprint).toBeDefined();
  });

  it("filters classes by name", async () => {
    const data = await uePost("/api/list-classes", { filter: "Actor", limit: 20 });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.count).toBeGreaterThan(0);
    // Every returned class should contain "Actor" in the name
    for (const cls of data.classes) {
      expect(cls.name.toLowerCase()).toContain("actor");
    }
  });

  it("filters classes by parent class", async () => {
    const data = await uePost("/api/list-classes", { parentClass: "Actor", limit: 20 });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.count).toBeGreaterThan(0);
  });

  it("returns error for non-existent parent class", async () => {
    const data = await uePost("/api/list-classes", { parentClass: "FakeClass_XYZ_999" });
    expect(data.error).toBeDefined();
  });

  it("respects limit parameter", async () => {
    const data = await uePost("/api/list-classes", { limit: 5 });
    expect(data.error).toBeUndefined();
    expect(data.classes.length).toBeLessThanOrEqual(5);
  });

  // --- list_functions ---

  it("lists functions on KismetSystemLibrary", async () => {
    const data = await uePost("/api/list-functions", { className: "KismetSystemLibrary" });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.count).toBeGreaterThan(0);
    // Should contain PrintString
    const names = data.functions.map((f: any) => f.name);
    expect(names).toContain("PrintString");
  });

  it("filters functions by name", async () => {
    const data = await uePost("/api/list-functions", {
      className: "KismetSystemLibrary",
      filter: "Print",
    });
    expect(data.error).toBeUndefined();
    expect(data.count).toBeGreaterThan(0);
    for (const fn of data.functions) {
      expect(fn.name.toLowerCase()).toContain("print");
    }
  });

  it("includes parameter info in function listing", async () => {
    const data = await uePost("/api/list-functions", {
      className: "KismetSystemLibrary",
      filter: "PrintString",
    });
    expect(data.error).toBeUndefined();
    const printFn = data.functions.find((f: any) => f.name === "PrintString");
    expect(printFn).toBeDefined();
    expect(printFn.parameters).toBeDefined();
    expect(printFn.parameters.length).toBeGreaterThan(0);
    // Verify parameter structure
    const param = printFn.parameters[0];
    expect(param.name).toBeDefined();
    expect(param.type).toBeDefined();
  });

  it("returns error for non-existent class", async () => {
    const data = await uePost("/api/list-functions", { className: "FakeClass_XYZ_999" });
    expect(data.error).toBeDefined();
  });

  it("returns error for missing className", async () => {
    const data = await uePost("/api/list-functions", {});
    expect(data.error).toBeDefined();
  });

  // --- list_properties ---

  it("lists properties on Actor", async () => {
    const data = await uePost("/api/list-properties", { className: "Actor" });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.count).toBeGreaterThan(0);
    // Verify structure
    const prop = data.properties[0];
    expect(prop.name).toBeDefined();
    expect(prop.type).toBeDefined();
  });

  it("filters properties by name", async () => {
    const data = await uePost("/api/list-properties", {
      className: "Actor",
      filter: "Root",
    });
    expect(data.error).toBeUndefined();
    for (const prop of data.properties) {
      expect(prop.name.toLowerCase()).toContain("root");
    }
  });

  it("returns error for non-existent class", async () => {
    const data = await uePost("/api/list-properties", { className: "FakeClass_XYZ_999" });
    expect(data.error).toBeDefined();
  });

  it("returns error for missing className", async () => {
    const data = await uePost("/api/list-properties", {});
    expect(data.error).toBeDefined();
  });
});
