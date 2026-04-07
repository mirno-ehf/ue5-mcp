import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("content browser tools", () => {
  describe("navigate_content_browser", () => {
    it("navigates to a valid path", async () => {
      const data = await uePost("/api/navigate-content-browser", {
        path: "/Game",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.navigatedTo).toBe("/Game");
    });

    it("rejects missing path", async () => {
      const data = await uePost("/api/navigate-content-browser", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("open_asset_editor", () => {
    it("returns error for non-existent asset", async () => {
      const data = await uePost("/api/open-asset-editor", {
        assetPath: "BP_Nonexistent_XYZ_999",
      });
      expect(data.error).toBeDefined();
    });

    it("rejects missing assetPath", async () => {
      const data = await uePost("/api/open-asset-editor", {});
      expect(data.error).toBeDefined();
    });
  });
});
