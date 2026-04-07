import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerActorQueryTools(server: McpServer): void {
  server.tool(
    "find_actors_by_tag",
    "Find all actors in the current level that have a specific tag. Requires editor mode.",
    { tag: z.string().describe("Tag to search for") },
    async ({ tag }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/find-actors-by-tag", { tag });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const lines = [`Found ${data.count} actor(s) with tag '${data.tag}':`];
      for (const a of data.actors ?? [])
        lines.push(`  ${a.label} (${a.class}) at (${a.location.x}, ${a.location.y}, ${a.location.z})`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "find_actors_by_class",
    "Find all actors of a specific class in the current level. Requires editor mode.",
    { className: z.string().describe("Class name to filter by (e.g. 'StaticMeshActor', 'PointLight')") },
    async ({ className }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/find-actors-by-class", { className });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const lines = [`Found ${data.count} actor(s) of class '${data.className}':`];
      for (const a of data.actors ?? [])
        lines.push(`  ${a.label} at (${a.location.x}, ${a.location.y}, ${a.location.z})`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "find_actors_in_radius",
    "Find all actors within a radius of a point in the current level. Requires editor mode.",
    {
      origin: z.object({
        x: z.number().describe("X coordinate of center point"),
        y: z.number().describe("Y coordinate of center point"),
        z: z.number().describe("Z coordinate of center point"),
      }).describe("Center point for the search"),
      radius: z.number().positive().describe("Search radius in Unreal units"),
    },
    async ({ origin, radius }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/find-actors-in-radius", { origin, radius });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const lines = [`Found ${data.count} actor(s) within radius ${data.radius}:`];
      for (const a of data.actors ?? [])
        lines.push(`  ${a.label} (${a.class}) - distance: ${Math.round(a.distance)}`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_actor_bounds",
    "Get the bounding box of an actor (origin + extent). Requires editor mode.",
    { actorLabel: z.string().describe("Label of the actor") },
    async ({ actorLabel }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/get-actor-bounds", { actorLabel });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const o = data.origin;
      const e = data.boxExtent;
      return { content: [{ type: "text" as const, text: `Bounds for '${data.actorLabel}':\n  Origin: (${o.x}, ${o.y}, ${o.z})\n  Extent: (${e.x}, ${e.y}, ${e.z})` }] };
    }
  );

  server.tool(
    "set_actor_tags",
    "Set tags on an actor (replaces existing tags). Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor"),
      tags: z.array(z.string()).describe("Array of tag strings to set"),
    },
    async ({ actorLabel, tags }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/set-actor-tags", { actorLabel, tags });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      return { content: [{ type: "text" as const, text: `Set ${data.tagCount} tag(s) on '${data.actorLabel}': ${(data.tags ?? []).join(", ")}\n\nNext steps:\n  1. Use find_actors_by_tag to verify` }] };
    }
  );
}
