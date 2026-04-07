import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("selection tools", () => {
  describe("get_editor_selection", () => {
    it("returns selection without errors", async () => {
      const data = await uePost("/api/get-editor-selection", {});
      expect(data.error).toBeUndefined();
      expect(typeof data.count).toBe("number");
      expect(Array.isArray(data.selectedActors)).toBe(true);
    });
  });

  describe("set_editor_selection", () => {
    it("returns error for missing actorLabels field", async () => {
      const data = await uePost("/api/set-editor-selection", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabels");
    });

    it("handles non-existent actors gracefully", async () => {
      const data = await uePost("/api/set-editor-selection", {
        actorLabels: ["NonExistent_Actor_XYZ_999"],
      });
      expect(data.error).toBeUndefined();
      expect(data.selectedCount).toBe(0);
      expect(data.notFoundCount).toBe(1);
    });

    it("accepts empty array", async () => {
      const data = await uePost("/api/set-editor-selection", {
        actorLabels: [],
      });
      expect(data.error).toBeUndefined();
      expect(data.selectedCount).toBe(0);
    });
  });

  describe("clear_selection", () => {
    it("clears selection without errors", async () => {
      const data = await uePost("/api/clear-selection", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(typeof data.previousSelectionCount).toBe("number");
    });
  });
});
