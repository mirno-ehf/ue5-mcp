import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet, uePost } from "../ue-bridge.js";
import { describeMaterial } from "../material-describe.js";

export function registerMaterialReadTools(server: McpServer): void {
  server.tool(
    "list_materials",
    "List all Material and Material Instance assets in the UE5 project. Filter by name/path and type.",
    {
      filter: z.string().optional().describe("Substring to match against material name or path"),
      type: z.enum(["all", "material", "instance"]).optional().default("all").describe("Filter by type: 'all' (default), 'material' (base materials only), 'instance' (material instances only)"),
    },
    async ({ filter, type: matType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/materials", {
        filter: filter || "",
        type: matType || "all",
      });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = data.materials.map(
        (mat: any) => `${mat.name} (${mat.path}) [${mat.type}]`
      );
      const summary = `Found ${data.count} of ${data.total} materials.\n\n${lines.join("\n")}`;
      return { content: [{ type: "text" as const, text: summary }] };
    }
  );

  server.tool(
    "get_material",
    "Get full details of a Material or Material Instance: domain, blend mode, shading model, parameters, expressions, referenced textures, usage flags, opacity clip value, texture sample count.",
    {
      name: z.string().describe("Material name or package path"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/material", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
    }
  );

  server.tool(
    "get_material_graph",
    "Get the material editor graph for a Material, with all expression nodes and connections.",
    {
      name: z.string().describe("Material name or package path"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/material-graph", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
    }
  );

  server.tool(
    "describe_material",
    "Get a human-readable description of a Material's graph, showing what feeds into each material input (BaseColor, Roughness, Normal, etc.).",
    {
      name: z.string().describe("Material name or package path"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/describe-material", { material: name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const text = data.description ? data.description : describeMaterial(data);
      return { content: [{ type: "text" as const, text }] };
    }
  );

  server.tool(
    "search_materials",
    "Search across Materials for expressions matching a query (parameter names, expression types).",
    {
      query: z.string().describe("Search term to match against expression types, parameter names, texture names"),
      maxResults: z.number().optional().default(50).describe("Maximum results to return"),
    },
    async ({ query, maxResults }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/search-materials", {
        query,
        maxResults: String(maxResults),
      });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Found ${data.resultCount} results for "${query}":\n`);

      for (const r of data.results) {
        let line = `[${r.material}] ${r.matchType}: ${r.matchValue}`;
        if (r.expressionType) line += ` (${r.expressionType})`;
        if (r.parameterName) line += ` param:${r.parameterName}`;
        lines.push(line);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "find_material_references",
    "Find all assets that reference a given Material or Material Instance.",
    {
      material: z.string().describe("Material name or package path"),
    },
    async ({ material }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/material-references", { material });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`References to: ${data.material || material}`);
      lines.push(`Total referencers: ${data.totalReferencers}`);

      if (data.referencers?.length) {
        lines.push("");
        for (const ref of data.referencers) {
          lines.push(`  ${ref}`);
        }
      }

      if (data.totalReferencers === 0) {
        lines.push("\nNo referencers found.");
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_material_functions",
    "List all Material Function assets in the project.",
    {
      filter: z.string().optional().describe("Substring to match against function name or path"),
    },
    async ({ filter }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/material-functions", { filter: filter || "" });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Found ${data.count} material functions.\n`);

      for (const fn of data.functions) {
        let line = `${fn.name} (${fn.path})`;
        if (fn.description) line += ` — ${fn.description}`;
        lines.push(line);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_material_function",
    "Get details of a Material Function: description, inputs, outputs, expressions.",
    {
      name: z.string().describe("Material Function name or package path"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/material-function", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
    }
  );
}
