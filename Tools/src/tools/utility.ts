import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { ensureUE, ueGet, isUEHealthy, gracefulShutdown, state } from "../ue-bridge.js";

export function registerUtilityTools(server: McpServer): void {
  server.tool(
    "server_status",
    "Check UE5 Blueprint server status. Starts the server if not running (blocks until ready).",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/health");
      return {
        content: [{
          type: "text" as const,
          text: `UE5 Blueprint server is running (${data.mode ?? (state.editorMode ? "editor" : "commandlet")} mode).\nBlueprints indexed: ${data.blueprintCount}\nMaps indexed: ${data.mapCount ?? "?"}`,
        }],
      };
    }
  );

  server.tool(
    "shutdown_server",
    "Shut down the UE5 Blueprint server to free memory (~2-4 GB). The server will auto-restart on the next blueprint tool call. Use this when done with blueprint analysis. Cannot shut down the editor — only the standalone commandlet.",
    {},
    async () => {
      if (state.editorMode) {
        return {
          content: [{
            type: "text" as const,
            text: "Connected to UE5 editor \u2014 cannot shut down the editor's MCP server. Close the editor to stop serving.",
          }],
        };
      }

      if (!state.ueProcess && !state.startupPromise && !(await isUEHealthy())) {
        return { content: [{ type: "text" as const, text: "UE5 server is already stopped." }] };
      }

      await gracefulShutdown();
      state.startupPromise = null;

      return {
        content: [{
          type: "text" as const,
          text: "UE5 Blueprint server shut down. Memory freed. It will auto-restart on the next blueprint tool call.",
        }],
      };
    }
  );
}
