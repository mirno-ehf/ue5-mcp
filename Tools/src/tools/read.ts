import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet } from "../ue-bridge.js";
import { summarizeBlueprint } from "../graph-describe.js";
import { describeGraph } from "../graph-describe.js";

export function registerReadTools(server: McpServer): void {
  server.tool(
    "list_blueprints",
    "List all Blueprint assets in the UE5 project, including level blueprints from .umap files. Optionally filter by name/path substring or parent class.",
    {
      filter: z.string().optional().describe("Substring to match against Blueprint name or path"),
      parentClass: z.string().optional().describe("Filter by parent class name"),
    },
    async ({ filter, parentClass }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/list", {
        filter: filter || "",
        parentClass: parentClass || "",
      });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = data.blueprints.map(
        (bp: any) => `${bp.name} (${bp.path}) [${bp.parentClass || "?"}]`
      );
      const summary = `Found ${data.count} of ${data.total} blueprints.\n\n${lines.join("\n")}`;
      return { content: [{ type: "text" as const, text: summary }] };
    }
  );

  server.tool(
    "get_blueprint",
    "Get full details of a specific Blueprint: variables, interfaces, and all graphs with nodes and connections. Also supports level blueprints from .umap files (e.g. 'MAP_Ward').",
    {
      name: z.string().describe("Blueprint name or package path (e.g. 'BP_Patient_Base', 'MAP_Ward')"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/blueprint", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
    }
  );

  server.tool(
    "get_blueprint_graph",
    "Get a specific named graph from a Blueprint (e.g. 'EventGraph', a function name). Graph names are URL-encoded automatically.",
    {
      name: z.string().describe("Blueprint name or package path"),
      graph: z.string().describe("Graph name (e.g. 'EventGraph')"),
    },
    async ({ name, graph }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      // ueGet uses URL.searchParams.set which handles encoding via encodeURIComponent (#8)
      const data = await ueGet("/api/graph", { name, graph });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableGraphs) msg += `\nAvailable: ${data.availableGraphs.join(", ")}`;
        return { content: [{ type: "text" as const, text: msg }] };
      }

      return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
    }
  );

  server.tool(
    "search_blueprints",
    "Search across Blueprints for nodes matching a query (function calls, events, variables). Loads BPs on demand so use 'path' filter to scope large searches.",
    {
      query: z.string().describe("Search term to match against node titles, function names, event names, variable names"),
      path: z.string().optional().describe("Filter to Blueprints whose path contains this substring (e.g. '/Game/Blueprints/Patients/')"),
      maxResults: z.number().optional().default(50).describe("Maximum results to return"),
    },
    async ({ query, path: pathFilter, maxResults }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/search", {
        query,
        path: pathFilter || "",
        maxResults: String(maxResults),
      });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = data.results.map(
        (r: any) =>
          `[${r.blueprint}] ${r.graph} > ${r.nodeTitle}` +
          (r.functionName ? ` fn:${r.functionName}` : "") +
          (r.eventName ? ` event:${r.eventName}` : "") +
          (r.variableName ? ` var:${r.variableName}` : "")
      );
      const summary = `Found ${data.resultCount} results for "${query}":\n\n${lines.join("\n")}`;
      return { content: [{ type: "text" as const, text: summary }] };
    }
  );

  server.tool(
    "get_blueprint_summary",
    "Get a concise human-readable summary of a Blueprint: variables with types, graphs with node counts, events, and function calls. Returns ~1-2K chars instead of 300K+ raw JSON. Use this first to understand a Blueprint before diving into specific graphs.",
    {
      name: z.string().describe("Blueprint name or package path (e.g. 'BPC_3LeadECG')"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/blueprint", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: summarizeBlueprint(data) }] };
    }
  );

  server.tool(
    "describe_graph",
    "Get a pseudo-code description of a specific Blueprint graph by walking execution pin chains. Shows the control flow as readable pseudo-code (IF/CALL/SET/SEQUENCE etc) with data flow annotations showing where each node gets its inputs. Use after get_blueprint_summary to understand a specific graph's logic. Graph names are URL-encoded automatically.",
    {
      name: z.string().describe("Blueprint name or package path"),
      graph: z.string().describe("Graph name (e.g. 'EventGraph', 'Set Connection Progress')"),
    },
    async ({ name, graph }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      // ueGet uses URL.searchParams.set which handles encoding via encodeURIComponent (#8)
      const data = await ueGet("/api/graph", { name, graph });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableGraphs) msg += `\nAvailable: ${data.availableGraphs.join(", ")}`;
        return { content: [{ type: "text" as const, text: msg }] };
      }

      return { content: [{ type: "text" as const, text: describeGraph(data) }] };
    }
  );

  server.tool(
    "find_asset_references",
    "Find all Blueprints (and other assets) that reference a given asset path. Equivalent to the editor's Reference Viewer. Use this to check dependencies before deleting assets or to map out which Blueprints use a specific struct, function library, or enum.",
    {
      assetPath: z.string().describe("Full asset path, e.g. '/Game/Blueprints/WebUI/S_Vitals'"),
    },
    async ({ assetPath }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/references", { assetPath });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`References to: ${data.assetPath}`);
      lines.push(`Total referencers: ${data.totalReferencers}`);

      if (data.blueprintReferencerCount > 0) {
        lines.push(`\nBlueprint referencers (${data.blueprintReferencerCount}):`);
        for (const ref of data.blueprintReferencers) {
          lines.push(`  ${ref}`);
        }
      }
      if (data.otherReferencerCount > 0) {
        lines.push(`\nOther referencers (${data.otherReferencerCount}):`);
        for (const ref of data.otherReferencers) {
          lines.push(`  ${ref}`);
        }
      }
      if (data.totalReferencers === 0) {
        lines.push("\nNo referencers found. Asset is safe to delete.");
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "search_by_type",
    "Find all usages of a specific type across Blueprints: variables, function/event parameters, Break/Make struct nodes. More granular than find_asset_references.",
    {
      typeName: z.string().describe("Type name to search for (e.g. 'FVitals', 'S_Vitals', 'ELungSound')"),
      filter: z.string().optional().describe("Optional path filter to scope the search (e.g. '/Game/Blueprints/')"),
    },
    async ({ typeName, filter }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const params: Record<string, string> = { typeName };
      if (filter) params.filter = filter;

      const data = await ueGet("/api/search-by-type", params);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Usages of type "${typeName}":`);

      if (data.variables?.length) {
        lines.push(`\nVariables (${data.variables.length}):`);
        for (const v of data.variables) {
          lines.push(`  ${v.blueprint}.${v.variableName}: ${v.type}`);
        }
      }

      if (data.parameters?.length) {
        lines.push(`\nFunction/Event Parameters (${data.parameters.length}):`);
        for (const p of data.parameters) {
          lines.push(`  ${p.blueprint}.${p.functionName}.${p.paramName}: ${p.type}`);
        }
      }

      if (data.structNodes?.length) {
        lines.push(`\nBreak/Make Struct Nodes (${data.structNodes.length}):`);
        for (const n of data.structNodes) {
          lines.push(`  ${n.blueprint} > ${n.graph} > ${n.nodeTitle} (${n.nodeId})`);
        }
      }

      if (data.otherUsages?.length) {
        lines.push(`\nOther Usages (${data.otherUsages.length}):`);
        for (const u of data.otherUsages) {
          lines.push(`  ${u.blueprint} > ${u.graph} > ${u.description}`);
        }
      }

      const total = (data.variables?.length || 0) + (data.parameters?.length || 0) +
        (data.structNodes?.length || 0) + (data.otherUsages?.length || 0);
      if (total === 0) {
        lines.push(`\nNo usages found.`);
      } else {
        lines.push(`\nTotal: ${total} usage(s)`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
