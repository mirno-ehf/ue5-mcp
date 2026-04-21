import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerEditorUtilityTools(server: McpServer): void {
  server.tool(
    "focus_actor",
    "Focus the viewport camera on a specific actor, centering it in view and selecting it. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor to focus on in the World Outliner"),
    },
    async ({ actorLabel }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/focus-actor", { actorLabel });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Focused on '${data.actorLabel}'`,
        `Location: (${data.location.x}, ${data.location.y}, ${data.location.z})`,
        `\nNext steps:`,
        `  1. The actor is now selected and centered in the viewport`,
        `  2. Use take_screenshot to capture the current view`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "editor_notification",
    "Show a toast notification in the UE5 editor. Useful for providing feedback to the user during long operations. Requires editor mode.",
    {
      message: z.string().describe("Notification message text"),
      severity: z.enum(["none", "success", "fail", "pending"]).optional()
        .describe("Visual style: none (default), success (green check), fail (red X), pending (spinner)"),
      duration: z.number().optional()
        .describe("How long to show the notification in seconds (default: 5)"),
    },
    async ({ message, severity, duration }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { message };
      if (severity) body.severity = severity;
      if (duration !== undefined) body.duration = duration;

      const data = await uePost("/api/editor-notification", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: `Notification shown: "${data.message}" (${data.duration}s)` }] };
    }
  );

  server.tool(
    "save_all",
    "Save all dirty (unsaved) packages in the editor, including maps and content. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/save-all", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        data.success ? "All dirty packages saved successfully." : "Save completed with some failures.",
        `\nNext steps:`,
        `  1. Use get_dirty_packages to verify no unsaved changes remain`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_dirty_packages",
    "List all packages with unsaved changes. Useful for checking what needs saving before closing. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/get-dirty-packages", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Unsaved packages: ${data.count}`);

      if (data.packages && data.packages.length > 0) {
        for (const pkg of data.packages) {
          lines.push(`  - ${pkg.name}`);
        }
      } else {
        lines.push("No unsaved changes.");
      }

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use save_all to save all dirty packages`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
