import { describe, it, expect } from "vitest";
import { uePost } from "../helpers.js";

describe("actor query tools", () => {
  describe("find_actors_by_tag", () => {
    it("returns error for missing tag field", async () => {
      const data = await uePost("/api/find-actors-by-tag", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("tag");
    });
  });

  describe("find_actors_by_class", () => {
    it("returns error for missing className field", async () => {
      const data = await uePost("/api/find-actors-by-class", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("className");
    });
  });

  describe("find_actors_in_radius", () => {
    it("returns error for missing origin field", async () => {
      const data = await uePost("/api/find-actors-in-radius", { radius: 100 });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("origin");
    });
    it("returns error for missing radius field", async () => {
      const data = await uePost("/api/find-actors-in-radius", { origin: { x: 0, y: 0, z: 0 } });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("radius");
    });
  });

  describe("get_actor_bounds", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/get-actor-bounds", {});
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/get-actor-bounds", { actorLabel: "NonExistent_XYZ_999" });
      expect(data.error).toBeDefined();
    });
  });

  describe("set_actor_tags", () => {
    it("returns error for missing actorLabel field", async () => {
      const data = await uePost("/api/set-actor-tags", { tags: ["test"] });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("actorLabel");
    });
    it("returns error for missing tags field", async () => {
      const data = await uePost("/api/set-actor-tags", { actorLabel: "SomeActor" });
      expect(data.error).toBeDefined();
      expect(data.error).toContain("tags");
    });
    it("returns error for non-existent actor", async () => {
      const data = await uePost("/api/set-actor-tags", { actorLabel: "NonExistent_XYZ_999", tags: ["test"] });
      expect(data.error).toBeDefined();
    });
  });
});
