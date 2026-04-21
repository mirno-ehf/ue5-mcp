import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerSublevelTools(server: McpServer): void {
  server.tool(
    "get_level_info",
    "Get information about the current editor world including persistent level details and all streaming sublevels. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/get-level-info", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`World: ${data.worldName}`);
      lines.push(`Map: ${data.mapName}`);
      if (data.persistentLevelActorCount !== undefined) {
        lines.push(`Persistent level actors: ${data.persistentLevelActorCount}`);
      }
      lines.push(`Streaming sublevels: ${data.streamingLevelCount}`);

      if (data.streamingLevels && data.streamingLevels.length > 0) {
        lines.push(`\nSublevels:`);
        for (const level of data.streamingLevels) {
          const status = level.isLoaded
            ? (level.isVisible ? "loaded, visible" : "loaded, hidden")
            : "unloaded";
          const actors = level.actorCount !== undefined ? `, ${level.actorCount} actors` : "";
          lines.push(`  - ${level.shortName} [${status}${actors}]`);
        }
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use list_sublevels for detailed sublevel information`);
      lines.push(`  2. Use load_sublevel / unload_sublevel to manage streaming levels`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_sublevels",
    "List all streaming sublevels in the current world with their load/visibility status, streaming class, and actor counts. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-sublevels", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Found ${data.count} sublevel(s)`);

      if (data.sublevels && data.sublevels.length > 0) {
        for (const level of data.sublevels) {
          lines.push(`\n  ${level.shortName}`);
          lines.push(`    Package: ${level.packageName}`);
          lines.push(`    Loaded: ${level.isLoaded}`);
          lines.push(`    Visible: ${level.isVisible}`);
          lines.push(`    Streaming class: ${level.streamingClass}`);
          if (level.actorCount !== undefined) {
            lines.push(`    Actors: ${level.actorCount}`);
          }
        }
      } else {
        lines.push("No streaming sublevels found in this world.");
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use load_sublevel to load an unloaded sublevel`);
      lines.push(`  2. Use unload_sublevel to unload a loaded sublevel`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "load_sublevel",
    "Load a streaming sublevel by name. Optionally make it visible immediately. Requires editor mode.",
    {
      levelName: z.string().describe("Package name or short name of the sublevel to load"),
      makeVisible: z.boolean().optional()
        .describe("Whether to also make the sublevel visible after loading (default: true)"),
    },
    async ({ levelName, makeVisible }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { levelName };
      if (makeVisible !== undefined) body.makeVisible = makeVisible;

      const data = await uePost("/api/load-sublevel", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Loaded sublevel '${data.levelName}'`,
        `Was loaded: ${data.wasLoaded}`,
        `Is loaded: ${data.isLoaded}`,
        `Is visible: ${data.isVisible}`,
        `\nNext steps:`,
        `  1. Use list_actors to see actors in the loaded sublevel`,
        `  2. Use unload_sublevel to unload it when no longer needed`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "unload_sublevel",
    "Unload a streaming sublevel by name. Hides and unloads the sublevel. Requires editor mode.",
    {
      levelName: z.string().describe("Package name or short name of the sublevel to unload"),
    },
    async ({ levelName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/unload-sublevel", { levelName });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Unloaded sublevel '${data.levelName}'`,
        `Was loaded: ${data.wasLoaded}`,
        `Is loaded: ${data.isLoaded}`,
        `\nNext steps:`,
        `  1. Use load_sublevel to reload the sublevel`,
        `  2. Use list_sublevels to verify the status`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
