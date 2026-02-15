import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { getUEHealth, gracefulShutdown, state } from "./ue-bridge.js";

// Tool registrations
import { registerReadTools } from "./tools/read.js";
import { registerMutationTools } from "./tools/mutation.js";
import { registerVariableTools } from "./tools/variables.js";
import { registerParamTools } from "./tools/params.js";
import { registerGraphTools } from "./tools/graphs.js";
import { registerInterfaceTools } from "./tools/interfaces.js";
import { registerDispatcherTools } from "./tools/dispatchers.js";
import { registerComponentTools } from "./tools/components.js";
import { registerSnapshotTools } from "./tools/snapshot.js";
import { registerValidationTools } from "./tools/validation.js";
import { registerUtilityTools } from "./tools/utility.js";
import { registerDiscoveryTools } from "./tools/discovery.js";

// Resource registrations
import { registerBlueprintListResource } from "./resources/blueprint-list.js";
import { registerWorkflowRecipesResource } from "./resources/workflow-recipes.js";

const server = new McpServer({
  name: "blueprint-mcp",
  version: "1.0.0",
});

// Register all tools
registerReadTools(server);
registerMutationTools(server);
registerVariableTools(server);
registerParamTools(server);
registerGraphTools(server);
registerInterfaceTools(server);
registerDispatcherTools(server);
registerComponentTools(server);
registerSnapshotTools(server);
registerValidationTools(server);
registerUtilityTools(server);
registerDiscoveryTools(server);

// Register resources
registerBlueprintListResource(server);
registerWorkflowRecipesResource(server);

// Cleanup on exit — only kill the commandlet if we spawned it (don't kill the editor).
process.on("exit", () => { if (!state.editorMode) state.ueProcess?.kill(); });
for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => {
    if (!state.editorMode && state.ueProcess) {
      await gracefulShutdown();
    }
    process.exit();
  });
}

async function main() {
  const health = await getUEHealth();
  if (health) {
    state.editorMode = health.mode === "editor";
    console.error(`Connected to UE5 ${health.mode} \u2014 MCP server already running.`);
  } else {
    state.editorMode = false;
    console.error("UE5 server not detected. Commandlet will be spawned on first tool call.");
  }

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
