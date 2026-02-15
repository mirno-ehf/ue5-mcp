import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerGraphTools(server: McpServer): void {
  server.tool(
    "reparent_blueprint",
    "Change a Blueprint's parent class. Can reparent to a C++ class (e.g. 'WebUIHUD') or another Blueprint. Compiles, refreshes all nodes, and saves.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'HUD_WebUIInterface')"),
      newParentClass: z.string().describe("New parent class name — C++ class (e.g. 'WebUIHUD') or Blueprint name"),
    },
    async ({ blueprint, newParentClass }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/reparent-blueprint", { blueprint, newParentClass });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint reparented successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Old parent: ${data.oldParentClass}`);
      lines.push(`New parent: ${data.newParentClass}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "create_blueprint",
    "Create a new Blueprint asset. Specify a parent class (C++ or Blueprint) and package path.",
    {
      blueprintName: z.string().describe("Name for the new Blueprint (e.g. 'BP_MyActor')"),
      packagePath: z.string().describe("Package path (e.g. '/Game/Blueprints/Actors')"),
      parentClass: z.string().describe("Parent class — C++ class (e.g. 'Actor', 'Pawn') or Blueprint name"),
      blueprintType: z.enum(["Normal", "Interface", "FunctionLibrary", "MacroLibrary"])
        .optional().default("Normal")
        .describe("Blueprint type (default: Normal)"),
    },
    async ({ blueprintName, packagePath, parentClass, blueprintType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/create-blueprint", { blueprintName, packagePath, parentClass, blueprintType });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint created successfully.`);
      lines.push(`Name: ${data.blueprintName}`);
      lines.push(`Path: ${data.assetPath}`);
      lines.push(`Parent: ${data.parentClass}`);
      lines.push(`Type: ${data.blueprintType}`);
      if (data.graphs?.length) lines.push(`Graphs: ${data.graphs.join(", ")}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  get_blueprint(blueprint="${data.blueprintName}") — inspect the new Blueprint`);
      lines.push(`  add_node(blueprint="${data.blueprintName}", ...) — add logic`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "create_graph",
    "Create a new function graph, macro graph, or custom event in a Blueprint. For function/macro, creates a new named graph with entry/exit nodes. For customEvent, adds a CustomEvent node to the EventGraph.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      graphName: z.string().describe("Name for the new graph or custom event"),
      graphType: z.enum(["function", "macro", "customEvent"]).describe("Type of graph to create: 'function' (new function graph), 'macro' (new macro graph), 'customEvent' (CustomEvent node in EventGraph)"),
    },
    async ({ blueprint, graphName, graphType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/create-graph", { blueprint, graphName, graphType });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Graph created successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Graph: ${data.graphName}`);
      lines.push(`Type: ${data.graphType}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      if (graphType === "customEvent") {
        lines.push(`  add_node(blueprint="${blueprint}", graph="EventGraph", ...) — add logic after the event`);
      } else {
        lines.push(`  add_node(blueprint="${blueprint}", graph="${graphName}", ...) — add nodes to the new graph`);
      }
      lines.push(`  get_blueprint_graph(blueprint="${blueprint}", graph="${graphName}") — inspect the graph`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
