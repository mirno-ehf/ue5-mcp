import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { ensureUE, ueGet, uePost, isUEHealthy, gracefulShutdown, state } from "../ue-bridge.js";

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
    "rescan_assets",
    "Re-scan the UE5 asset registry and refresh the server's cached asset lists. Use this if newly created assets are not appearing in list_blueprints/list_materials, or if the server started before the editor finished loading assets.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/rescan", {});
      if (data.error) {
        return { content: [{ type: "text" as const, text: `Rescan failed: ${data.error}` }] };
      }

      const lines = [
        "Asset registry rescanned.",
        `Blueprints: ${data.blueprintCount}${data.delta?.blueprints ? ` (${data.delta.blueprints >= 0 ? "+" : ""}${data.delta.blueprints})` : ""}`,
        `Maps: ${data.mapCount}${data.delta?.maps ? ` (${data.delta.maps >= 0 ? "+" : ""}${data.delta.maps})` : ""}`,
        `Materials: ${data.materialCount}${data.delta?.materials ? ` (${data.delta.materials >= 0 ? "+" : ""}${data.delta.materials})` : ""}`,
        `Material Instances: ${data.materialInstanceCount}${data.delta?.materialInstances ? ` (${data.delta.materialInstances >= 0 ? "+" : ""}${data.delta.materialInstances})` : ""}`,
        `Material Functions: ${data.materialFunctionCount}${data.delta?.materialFunctions ? ` (${data.delta.materialFunctions >= 0 ? "+" : ""}${data.delta.materialFunctions})` : ""}`,
      ];
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
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
