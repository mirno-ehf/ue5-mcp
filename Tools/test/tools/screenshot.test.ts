import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("screenshot tools", () => {
  describe("take_screenshot", () => {
    it("captures a screenshot with default filename", async () => {
      const data = await uePost("/api/take-screenshot", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.filename).toBeDefined();
      expect(data.fullPath).toBeDefined();
      expect(typeof data.width).toBe("number");
      expect(typeof data.height).toBe("number");
    });

    it("captures a screenshot with custom filename", async () => {
      const data = await uePost("/api/take-screenshot", {
        filename: "test_screenshot.png",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.filename).toBe("test_screenshot.png");
    });
  });

  describe("take_high_res_screenshot", () => {
    it("requests a high-res screenshot with default multiplier", async () => {
      const data = await uePost("/api/take-high-res-screenshot", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.resolutionMultiplier).toBe(2);
      expect(typeof data.estimatedWidth).toBe("number");
      expect(typeof data.estimatedHeight).toBe("number");
    });

    it("accepts custom resolution multiplier", async () => {
      const data = await uePost("/api/take-high-res-screenshot", {
        resolutionMultiplier: 4,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.resolutionMultiplier).toBe(4);
    });
  });
});
