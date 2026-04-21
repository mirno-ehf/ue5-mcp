import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerContentBrowserTools(server: McpServer): void {
  server.tool(
    "navigate_content_browser",
    "Navigate the Content Browser to a specific folder path. Useful for browsing assets in a particular directory. Requires editor mode.",
    {
      path: z.string().describe("Content path to navigate to, e.g. '/Game/Blueprints' or '/Game/Materials'"),
    },
    async ({ path }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/navigate-content-browser", { path });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Content Browser navigated to: ${data.navigatedTo}`,
        `\nNext steps:`,
        `  1. Use list_blueprints to see assets in this folder`,
        `  2. Use open_asset_editor to open a specific asset`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "open_asset_editor",
    "Open an asset in its dedicated editor (Blueprint editor, Material editor, etc.). Requires editor mode.",
    {
      assetPath: z.string().describe("Asset name or full package path (e.g. 'BP_MyActor' or '/Game/Blueprints/BP_MyActor')"),
    },
    async ({ assetPath }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/open-asset-editor", { assetPath });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `${data.success ? "Opened" : "Failed to open"} editor for '${data.assetName}'`,
        `  Path: ${data.assetPath}`,
        `  Class: ${data.assetClass}`,
      ];

      if (data.warning) {
        lines.push(`  Warning: ${data.warning}`);
      }

      lines.push(
        `\nNext steps:`,
        `  1. Use get_blueprint to inspect the asset's contents`,
        `  2. Use navigate_content_browser to browse related assets`,
      );

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
