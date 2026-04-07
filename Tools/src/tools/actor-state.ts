import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerActorStateTools(server: McpServer): void {
  server.tool(
    "set_actor_mobility",
    "Set an actor's mobility type (Static, Stationary, or Movable). This affects whether the actor can move at runtime and what lighting features are available. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor in the World Outliner"),
      mobility: z.enum(["Static", "Stationary", "Movable"])
        .describe("Mobility type: Static (best perf, no movement), Stationary (some movement), Movable (full movement)"),
    },
    async ({ actorLabel, mobility }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-actor-mobility", { actorLabel, mobility });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Set mobility of '${data.actorLabel}'`,
        `Previous: ${data.previousMobility}`,
        `New: ${data.newMobility}`,
        `\nNext steps:`,
        `  1. Use list_actors to verify the change`,
        `  2. Static actors cannot move at runtime — use Movable if the actor needs to move`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_actor_visibility",
    "Show or hide an actor in the level. Sets both editor visibility and in-game visibility. Optionally propagates to attached child actors. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor in the World Outliner"),
      visible: z.boolean().describe("true to show the actor, false to hide it"),
      propagateToChildren: z.boolean().optional()
        .describe("Whether to also show/hide attached child actors (default: true)"),
    },
    async ({ actorLabel, visible, propagateToChildren }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { actorLabel, visible };
      if (propagateToChildren !== undefined) body.propagateToChildren = propagateToChildren;

      const data = await uePost("/api/set-actor-visibility", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `${data.visible ? "Shown" : "Hidden"} actor '${data.actorLabel}'`,
        `Was hidden: ${data.wasHidden}`,
        `Propagated to children: ${data.propagatedToChildren}`,
        `\nNext steps:`,
        `  1. Use list_actors to see all actors and their visibility state`,
        `  2. Use set_actor_visibility again to toggle back`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_actor_physics",
    "Enable or disable physics simulation on an actor's primitive component. Automatically sets mobility to Movable when enabling physics. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor in the World Outliner"),
      simulatePhysics: z.boolean().describe("true to enable physics simulation, false to disable"),
      enableGravity: z.boolean().optional()
        .describe("Whether gravity affects this actor (default: true)"),
    },
    async ({ actorLabel, simulatePhysics, enableGravity }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { actorLabel, simulatePhysics };
      if (enableGravity !== undefined) body.enableGravity = enableGravity;

      const data = await uePost("/api/set-actor-physics", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `${data.simulatePhysics ? "Enabled" : "Disabled"} physics on '${data.actorLabel}'`,
        `Was simulating: ${data.wasSimulating}`,
        `Component: ${data.component}`,
        `\nNext steps:`,
        `  1. Use set_actor_mobility to change mobility if needed`,
        `  2. Physics requires a collision-enabled primitive component (StaticMesh, etc.)`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
