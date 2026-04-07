import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

const Vec3Schema = z.object({
  x: z.number().describe("X coordinate"),
  y: z.number().describe("Y coordinate"),
  z: z.number().describe("Z coordinate"),
});

export function registerSpatialTools(server: McpServer): void {
  server.tool(
    "raycast",
    "Perform a line trace (raycast) from point A to point B in the editor world. Returns hit information including the actor, component, impact point, and surface normal. Supports single and multi-hit modes. Requires editor mode.",
    {
      start: Vec3Schema.describe("Start point of the ray (world coordinates)"),
      end: Vec3Schema.describe("End point of the ray (world coordinates)"),
      channel: z.enum(["Visibility", "Camera", "WorldStatic", "WorldDynamic", "Pawn", "PhysicsBody"]).optional()
        .describe("Collision channel to trace against (default: Visibility)"),
      traceComplex: z.boolean().optional()
        .describe("Whether to trace against complex collision geometry (default: false)"),
      multi: z.boolean().optional()
        .describe("Whether to return all hits along the ray, not just the first (default: false)"),
    },
    async ({ start, end, channel, traceComplex, multi }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { start, end };
      if (channel) body.channel = channel;
      if (traceComplex !== undefined) body.traceComplex = traceComplex;
      if (multi !== undefined) body.multi = multi;

      const data = await uePost("/api/raycast", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Raycast from (${data.traceStart.x}, ${data.traceStart.y}, ${data.traceStart.z}) to (${data.traceEnd.x}, ${data.traceEnd.y}, ${data.traceEnd.z})`);

      if (!data.hit) {
        lines.push("Result: No hit");
      } else if (data.hits) {
        // Multi-hit mode
        lines.push(`Result: ${data.hitCount} hit(s)`);
        for (const hit of data.hits) {
          lines.push(`  ---`);
          if (hit.actorLabel) lines.push(`  Actor: ${hit.actorLabel} (${hit.actorClass})`);
          if (hit.componentName) lines.push(`  Component: ${hit.componentName}`);
          lines.push(`  Impact: (${hit.impactPoint.x.toFixed(1)}, ${hit.impactPoint.y.toFixed(1)}, ${hit.impactPoint.z.toFixed(1)})`);
          lines.push(`  Normal: (${hit.impactNormal.x.toFixed(2)}, ${hit.impactNormal.y.toFixed(2)}, ${hit.impactNormal.z.toFixed(2)})`);
          lines.push(`  Distance: ${hit.distance.toFixed(1)}`);
          if (hit.physicalMaterial) lines.push(`  Material: ${hit.physicalMaterial}`);
        }
      } else {
        // Single hit
        lines.push("Result: Hit!");
        if (data.actorLabel) lines.push(`Actor: ${data.actorLabel} (${data.actorClass})`);
        if (data.componentName) lines.push(`Component: ${data.componentName}`);
        lines.push(`Impact: (${data.impactPoint.x.toFixed(1)}, ${data.impactPoint.y.toFixed(1)}, ${data.impactPoint.z.toFixed(1)})`);
        lines.push(`Normal: (${data.impactNormal.x.toFixed(2)}, ${data.impactNormal.y.toFixed(2)}, ${data.impactNormal.z.toFixed(2)})`);
        lines.push(`Distance: ${data.distance.toFixed(1)}`);
        if (data.physicalMaterial) lines.push(`Material: ${data.physicalMaterial}`);
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use the impact point coordinates for spawning actors or setting transforms`);
      lines.push(`  2. Use multi mode to find all actors along a path`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
