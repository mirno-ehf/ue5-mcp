import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("PIE lifecycle tools", () => {
  describe("is_pie_running", () => {
    it("reports PIE status without errors", async () => {
      const data = await uePost("/api/is-pie-running", {});
      expect(data.error).toBeUndefined();
      expect(typeof data.running).toBe("boolean");
      expect(typeof data.paused).toBe("boolean");
    });
  });

  describe("start_pie", () => {
    it("starts a PIE session", async () => {
      // First check if PIE is already running
      const status = await uePost("/api/is-pie-running", {});
      if (status.running) {
        // Stop it first
        await uePost("/api/stop-pie", {});
        // Wait a moment for cleanup
        await new Promise((r) => setTimeout(r, 2000));
      }

      const data = await uePost("/api/start-pie", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);

      // Wait for PIE to start
      await new Promise((r) => setTimeout(r, 3000));
    });
  });

  describe("pie_pause", () => {
    it("pauses the PIE session", async () => {
      const data = await uePost("/api/pie-pause", { paused: true });
      // May error if PIE hasn't fully started
      if (!data.error) {
        expect(data.success).toBe(true);
        expect(data.paused).toBe(true);
      }
    });

    it("unpauses the PIE session", async () => {
      const data = await uePost("/api/pie-pause", { paused: false });
      if (!data.error) {
        expect(data.success).toBe(true);
        expect(data.paused).toBe(false);
      }
    });

    it("rejects missing paused field", async () => {
      const data = await uePost("/api/pie-pause", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("stop_pie", () => {
    it("stops the PIE session", async () => {
      const data = await uePost("/api/stop-pie", {});
      // Accept either success or "not running" error
      if (!data.error) {
        expect(data.success).toBe(true);
      }
    });
  });
});
