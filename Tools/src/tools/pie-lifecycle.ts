import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerPIELifecycleTools(server: McpServer): void {
  server.tool(
    "start_pie",
    "Start a Play In Editor (PIE) session. Launches the game in the editor viewport for testing. Requires editor mode and no active PIE session.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/start-pie", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `PIE session started.`,
        `  Status: ${data.status}`,
        `\nNext steps:`,
        `  1. Use is_pie_running to check when the session is fully active`,
        `  2. Use pie_pause to pause/unpause execution`,
        `  3. Use stop_pie to end the session`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "stop_pie",
    "Stop the active Play In Editor (PIE) session. Returns the editor to edit mode. Requires a running PIE session.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/stop-pie", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: `PIE session stopped. ${data.status}` }] };
    }
  );

  server.tool(
    "is_pie_running",
    "Check whether a Play In Editor (PIE) session is currently active and whether it is paused. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/is-pie-running", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const status = data.running
        ? (data.paused ? "Running (PAUSED)" : "Running")
        : "Not running";

      return { content: [{ type: "text" as const, text: `PIE status: ${status}` }] };
    }
  );

  server.tool(
    "pie_pause",
    "Pause or unpause the active PIE session. Useful for inspecting game state at a specific moment. Requires a running PIE session.",
    {
      paused: z.boolean().describe("true to pause, false to unpause"),
    },
    async ({ paused }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/pie-pause", { paused });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: `PIE ${data.paused ? "paused" : "unpaused"}.` }] };
    }
  );
}
