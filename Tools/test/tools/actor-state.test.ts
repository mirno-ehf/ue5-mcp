import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("actor state tools", () => {
  describe("set_actor_mobility", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/set-actor-mobility", { mobility: "Static" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });

    it("returns error for missing mobility field", async () => {
      const data = await uePost("/api/set-actor-mobility", { actorLabel: "SomeActor" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("mobility");
    });

    it("returns error for invalid mobility value", async () => {
      const data = await uePost("/api/set-actor-mobility", {
        actorLabel: "SomeActor",
        mobility: "InvalidValue",
      });
      expect(data.error).toBeDefined();
    });

    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/set-actor-mobility", {
        actorLabel: "NonExistent_Actor_XYZ_999",
        mobility: "Movable",
      });
      expect(data.error).toBeDefined();
    });

    it("rejects empty JSON body", async () => {
      const data = await uePost("/api/set-actor-mobility", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("set_actor_visibility", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/set-actor-visibility", { visible: true });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });

    it("returns error for missing visible field", async () => {
      const data = await uePost("/api/set-actor-visibility", { actorLabel: "SomeActor" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("visible");
    });

    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/set-actor-visibility", {
        actorLabel: "NonExistent_Actor_XYZ_999",
        visible: false,
      });
      expect(data.error).toBeDefined();
    });

    it("rejects empty JSON body", async () => {
      const data = await uePost("/api/set-actor-visibility", {});
      expect(data.error).toBeDefined();
    });
  });

  describe("set_actor_physics", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/set-actor-physics", { simulatePhysics: true });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });

    it("returns error for missing simulatePhysics field", async () => {
      const data = await uePost("/api/set-actor-physics", { actorLabel: "SomeActor" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("simulatePhysics");
    });

    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/set-actor-physics", {
        actorLabel: "NonExistent_Actor_XYZ_999",
        simulatePhysics: true,
      });
      expect(data.error).toBeDefined();
    });

    it("rejects empty JSON body", async () => {
      const data = await uePost("/api/set-actor-physics", {});
      expect(data.error).toBeDefined();
    });
  });
});
