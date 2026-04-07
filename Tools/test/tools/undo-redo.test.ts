import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("undo/redo tools", () => {
  describe("undo", () => {
    it("returns appropriate response when nothing to undo", async () => {
      // In a fresh commandlet there may be nothing to undo
      const data = await uePost("/api/undo", {});
      // Either succeeds or returns an error about nothing to undo
      if (data.error) {
        expect(data.error).toContain("undo");
      } else {
        expect(data.success).toBe(true);
      }
    });
  });

  describe("redo", () => {
    it("returns appropriate response when nothing to redo", async () => {
      const data = await uePost("/api/redo", {});
      if (data.error) {
        expect(data.error).toContain("redo");
      } else {
        expect(data.success).toBe(true);
      }
    });
  });

  describe("begin_transaction", () => {
    it("returns error for missing description field", async () => {
      const data = await uePost("/api/begin-transaction", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("description");
    });

    it("succeeds with valid description", async () => {
      const data = await uePost("/api/begin-transaction", {
        description: "Test transaction",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.transactionIndex).toBeDefined();

      // End the transaction to clean up
      const endData = await uePost("/api/end-transaction", {});
      expect(endData.error).toBeUndefined();
    });
  });

  describe("end_transaction", () => {
    it("can end a transaction", async () => {
      // Start a transaction first
      await uePost("/api/begin-transaction", { description: "Test end" });
      const data = await uePost("/api/end-transaction", {});
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
    });
  });
});
