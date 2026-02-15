import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { isUEHealthy, ueGet } from "../ue-bridge.js";

export function registerBlueprintListResource(server: McpServer): void {
  server.resource(
    "blueprint-list",
    "blueprint:///list",
    { description: "List of all exported Blueprints", mimeType: "application/json" },
    async (uri) => {
      if (!(await isUEHealthy())) {
        return { contents: [{ uri: uri.href, text: "[]", mimeType: "application/json" }] };
      }
      const data = await ueGet("/api/list");
      return { contents: [{ uri: uri.href, text: JSON.stringify(data.blueprints, null, 2), mimeType: "application/json" }] };
    }
  );
}
