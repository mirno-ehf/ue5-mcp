import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("editor utility tools", () => {
  describe("focus_actor", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/focus-actor", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });

    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/focus-actor", {
        actorLabel: "NonExistent_Actor_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("editor_notification", () => {
    it("returns error for missing message field", async () => {
      const data = await uePost("/api/editor-notification", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("message");
    });

    it("succeeds with valid message", async () => {
      const data = await uePost("/api/editor-notification", {
        message: "Test notification from MCP",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.message).toBe("Test notification from MCP");
    });
  });

  describe("save_all", () => {
    it("completes without errors", async () => {
      const data = await uePost("/api/save-all", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBeDefined();
    });
  });

  describe("get_dirty_packages", () => {
    it("returns package list without errors", async () => {
      const data = await uePost("/api/get-dirty-packages", {});
      expect(data.error).toBeUndefined();
      expect(typeof data.count).toBe("number");
      expect(Array.isArray(data.packages)).toBe(true);
    });
  });
});
