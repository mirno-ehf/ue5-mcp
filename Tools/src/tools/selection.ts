import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerSelectionTools(server: McpServer): void {
  server.tool(
    "get_editor_selection",
    "Get the currently selected actors in the editor. Returns labels, classes, and locations. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/get-editor-selection", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Selected actors: ${data.count}`);

      if (data.selectedActors && data.selectedActors.length > 0) {
        for (const actor of data.selectedActors) {
          lines.push(`  - ${actor.label} (${actor.class}) at (${actor.location.x}, ${actor.location.y}, ${actor.location.z})`);
        }
      } else {
        lines.push("No actors selected.");
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use set_editor_selection to change the selection`);
      lines.push(`  2. Use clear_selection to deselect all`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_editor_selection",
    "Select specific actors by label. Clears the current selection first. Requires editor mode.",
    {
      actorLabels: z.array(z.string()).describe("Array of actor labels to select"),
    },
    async ({ actorLabels }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-editor-selection", { actorLabels });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Selected ${data.selectedCount} actor(s)`,
      ];

      if (data.selected && data.selected.length > 0) {
        lines.push(`Selected: ${data.selected.join(", ")}`);
      }

      if (data.notFoundCount > 0) {
        lines.push(`Not found (${data.notFoundCount}): ${data.notFound.join(", ")}`);
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use get_editor_selection to verify the selection`);
      lines.push(`  2. Use focus_actor to focus on a selected actor`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "clear_selection",
    "Deselect all currently selected actors in the editor. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/clear-selection", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Selection cleared`,
        `Previously selected: ${data.previousSelectionCount} actor(s)`,
        `\nNext steps:`,
        `  1. Use set_editor_selection to select new actors`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
