import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("level actor tools", () => {
  describe("attach_actor", () => {
    it("returns error for missing childActor field", async () => {
      const data = await uePost("/api/attach-actor", { parentActor: "SomeParent" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("childActor");
    });
    it("returns error for missing parentActor field", async () => {
      const data = await uePost("/api/attach-actor", { childActor: "SomeChild" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("parentActor");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/attach-actor", {
        childActor: "NonExistent_Child_XYZ_999",
        parentActor: "NonExistent_Parent_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });
    it("rejects empty JSON body", async () => {
      const data = await uePost("/api/attach-actor", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("detach_actor", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/detach-actor", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/detach-actor", { actorLabel: "NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });
  });

  describe("duplicate_actor", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/duplicate-actor", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/duplicate-actor", { actorLabel: "NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });
  });

  describe("rename_actor", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/rename-actor", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });
    it("returns error for missing newLabel field", async () => {
      const data = await uePost("/api/rename-actor", { actorLabel: "SomeActor" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("newLabel");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/rename-actor", { actorLabel: "NonExistent_XYZ_999", newLabel: "New" });
      expect(data.error).toBeDefined();
    });
  });
});
