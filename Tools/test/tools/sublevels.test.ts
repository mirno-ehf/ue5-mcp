import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("sublevel tools", () => {
  describe("get_level_info", () => {
    it("returns world information without errors", async () => {
      const data = await uePost("/api/get-level-info", {});
      expect(data.error).toBeUndefined();
      expect(data.worldName).toBeDefined();
      expect(data.mapName).toBeDefined();
      expect(typeof data.streamingLevelCount).toBe("number");
    });
  });

  describe("list_sublevels", () => {
    it("returns sublevel list without errors", async () => {
      const data = await uePost("/api/list-sublevels", {});
      expect(data.error).toBeUndefined();
      expect(typeof data.count).toBe("number");
      expect(Array.isArray(data.sublevels)).toBe(true);
    });
  });

  describe("load_sublevel", () => {
    it("returns error for missing levelName field", async () => {
      const data = await uePost("/api/load-sublevel", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("levelName");
    });

    it("returns error for non-existent sublevel", async () => {
      const data = await uePost("/api/load-sublevel", {
        levelName: "NonExistent_Sublevel_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("unload_sublevel", () => {
    it("returns error for missing levelName field", async () => {
      const data = await uePost("/api/unload-sublevel", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("levelName");
    });

    it("returns error for non-existent sublevel", async () => {
      const data = await uePost("/api/unload-sublevel", {
        levelName: "NonExistent_Sublevel_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });
  });
});
