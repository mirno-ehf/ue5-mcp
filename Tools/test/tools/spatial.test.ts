import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("spatial tools", () => {
  describe("raycast", () => {
    it("returns error for missing start field", async () => {
      const data = await uePost("/api/raycast", {
        end: { x: 100, y: 0, z: 0 },
      });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("start");
    });

    it("returns error for missing end field", async () => {
      const data = await uePost("/api/raycast", {
        start: { x: 0, y: 0, z: 0 },
      });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("end");
    });

    it("rejects empty JSON body", async () => {
      const data = await uePost("/api/raycast", {});
      expect(data.error).toBeDefined();
    });

    it("returns hit=false for a trace into empty space", async () => {
      // Trace high above the level where nothing should exist
      const data = await uePost("/api/raycast", {
        start: { x: 0, y: 0, z: 999999 },
        end: { x: 0, y: 0, z: 999998 },
      });
      // Should succeed (no error) but hit=false
      expect(data.error).toBeUndefined();
      expect(data.hit).toBe(false);
    });

    it("supports multi-hit mode without errors", async () => {
      const data = await uePost("/api/raycast", {
        start: { x: 0, y: 0, z: 1000 },
        end: { x: 0, y: 0, z: -1000 },
        multi: true,
      });
      expect(data.error).toBeUndefined();
      expect(data.hitCount).toBeDefined();
      expect(typeof data.hitCount).toBe("number");
    });
  });
});
