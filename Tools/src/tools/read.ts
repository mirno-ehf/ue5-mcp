import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet, uePost } from "../ue-bridge.js";
import { summarizeBlueprint } from "../graph-describe.js";
import { describeGraph } from "../graph-describe.js";

export function registerReadTools(server: McpServer): void {
  server.tool(
    "list_blueprints",
    "List all Blueprint assets in the UE5 project, including level blueprints from .umap files. Optionally filter by name/path substring, parent class, or type (regular vs level).",
    {
      filter: z.string().optional().describe("Substring to match against Blueprint name or path"),
      parentClass: z.string().optional().describe("Filter by parent class name"),
      type: z.enum(["all", "regular", "level"]).optional().default("all").describe("Filter by blueprint type: 'all' (default), 'regular' (standard BPs only), 'level' (level blueprints only)"),
    },
    async ({ filter, parentClass, type: bpType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/list", {
        filter: filter || "",
        parentClass: parentClass || "",
        type: bpType || "all",
      });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = data.blueprints.map(
        (bp: any) => {
          const levelTag = bp.isLevelBlueprint ? " Level" : "";
          return `${bp.name} (${bp.path}) [${bp.parentClass || "?"}${levelTag}]`;
        }
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
        (r: any) => {
          const levelTag = r.isLevelBlueprint ? " Level" : "";
          return `[${r.blueprint}${levelTag}] ${r.graph} > ${r.nodeTitle}` +
            (r.functionName ? ` fn:${r.functionName}` : "") +
            (r.eventName ? ` event:${r.eventName}` : "") +
            (r.variableName ? ` var:${r.variableName}` : "");
        }
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

      // C++ returns a flat `results` array with a `usage` field on each entry.
      // Categorize by usage type for readable output.
      const results: any[] = data.results || [];
      const variables = results.filter((r: any) => r.usage === "variable");
      const funcParams = results.filter((r: any) => r.usage === "functionParameter");
      const eventParams = results.filter((r: any) => r.usage === "eventParameter");
      const breakStructs = results.filter((r: any) => r.usage === "breakStruct");
      const makeStructs = results.filter((r: any) => r.usage === "makeStruct");
      const pinConns = results.filter((r: any) => r.usage === "pinConnection");

      const tag = (r: any) => r.isLevelBlueprint ? " Level" : "";

      const lines: string[] = [];
      lines.push(`Usages of type "${typeName}" (${data.resultCount} result(s)):`);

      if (variables.length) {
        lines.push(`\nVariables (${variables.length}):`);
        for (const v of variables) {
          lines.push(`  [${v.blueprint}${tag(v)}] ${v.location}: ${v.currentType}${v.currentSubtype ? `<${v.currentSubtype}>` : ""}`);
        }
      }

      if (funcParams.length) {
        lines.push(`\nFunction Parameters (${funcParams.length}):`);
        for (const p of funcParams) {
          lines.push(`  [${p.blueprint}${tag(p)}] ${p.location}: ${p.currentType}${p.currentSubtype ? `<${p.currentSubtype}>` : ""}`);
        }
      }

      if (eventParams.length) {
        lines.push(`\nEvent Parameters (${eventParams.length}):`);
        for (const p of eventParams) {
          lines.push(`  [${p.blueprint}${tag(p)}] ${p.location}: ${p.currentType}${p.currentSubtype ? `<${p.currentSubtype}>` : ""}`);
        }
      }

      if (breakStructs.length) {
        lines.push(`\nBreak Struct Nodes (${breakStructs.length}):`);
        for (const n of breakStructs) {
          lines.push(`  [${n.blueprint}${tag(n)}] ${n.location} (${n.structType})`);
        }
      }

      if (makeStructs.length) {
        lines.push(`\nMake Struct Nodes (${makeStructs.length}):`);
        for (const n of makeStructs) {
          lines.push(`  [${n.blueprint}${tag(n)}] ${n.location} (${n.structType})`);
        }
      }

      if (pinConns.length) {
        lines.push(`\nPin Connections (${pinConns.length}):`);
        for (const p of pinConns) {
          lines.push(`  [${p.blueprint}${tag(p)}] ${p.graph} > ${p.location} (${p.connectionCount} connection(s))`);
        }
      }

      if (results.length === 0) {
        lines.push(`\nNo usages found.`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_skeleton",
    "Inspect a USkeleton asset: dumps the full bone hierarchy (with parent index, ref-pose transform), all sockets, and the curve metadata name list. Use the package path (e.g. '/Game/Characters/CC/Backend/CC4/CC5_Rig'). Useful for diffing rigs across characters.",
    {
      path: z.string().describe("Package path of the USkeleton asset, e.g. '/Game/Characters/CC/Backend/CC4/CC5_Rig'"),
      tree: z.boolean().optional().default(true).describe("If true (default), format bones as an indented hierarchy tree. If false, return raw JSON."),
      includeTransforms: z.boolean().optional().default(false).describe("Include ref-pose location in tree output (off by default to keep it compact)."),
    },
    async ({ path, tree, includeTransforms }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/skeleton", { path });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      if (!tree) {
        return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
      }

      const bones: Array<{
        index: number;
        name: string;
        parentIndex: number;
        parentName: string;
        locX?: number; locY?: number; locZ?: number;
      }> = data.bones || [];

      const children = new Map<number, number[]>();
      for (const b of bones) {
        const p = b.parentIndex;
        if (!children.has(p)) children.set(p, []);
        children.get(p)!.push(b.index);
      }

      const lines: string[] = [];
      lines.push(`Skeleton: ${data.name} (${data.path})`);
      lines.push(`Bones: ${data.boneCount}   Sockets: ${data.socketCount}   Curves: ${data.curveCount}`);
      lines.push("");
      lines.push("Bone hierarchy:");

      const walk = (idx: number, depth: number) => {
        const b = bones[idx];
        if (!b) return;
        const indent = "  ".repeat(depth);
        let suffix = "";
        if (includeTransforms && b.locX !== undefined) {
          suffix = `  [loc ${b.locX!.toFixed(2)}, ${b.locY!.toFixed(2)}, ${b.locZ!.toFixed(2)}]`;
        }
        lines.push(`${indent}${b.name}${suffix}`);
        const kids = children.get(idx) || [];
        for (const k of kids) walk(k, depth + 1);
      };
      const roots = children.get(-1) || [];
      for (const r of roots) walk(r, 0);

      if (data.sockets && data.sockets.length) {
        lines.push("");
        lines.push(`Sockets (${data.sockets.length}):`);
        for (const s of data.sockets) {
          const loc = (s.locX !== undefined)
            ? `  loc(${s.locX.toFixed(2)}, ${s.locY.toFixed(2)}, ${s.locZ.toFixed(2)})`
            : "";
          lines.push(`  ${s.name} @ ${s.bone}${loc}`);
        }
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_skeleton_socket",
    "Add (or update) a single socket on a USkeleton asset. The skeleton .uasset is saved to disk; the read-only attribute is cleared automatically. Wrapped in an undo transaction. Use 'overwrite=false' to refuse if a socket with the same name already exists. Use 'dryRun=true' to preview without saving.",
    {
      path: z.string().describe("Package path of the USkeleton, e.g. '/Game/Characters/CC/Backend/CC4/CC5New_Rig'"),
      socketName: z.string().describe("Socket name (FName) to create or update"),
      bone: z.string().describe("Bone name the socket is parented to. Must exist on the skeleton."),
      locX: z.number().optional().default(0),
      locY: z.number().optional().default(0),
      locZ: z.number().optional().default(0),
      rotPitch: z.number().optional().default(0),
      rotYaw: z.number().optional().default(0),
      rotRoll: z.number().optional().default(0),
      scaleX: z.number().optional().default(1),
      scaleY: z.number().optional().default(1),
      scaleZ: z.number().optional().default(1),
      overwrite: z.boolean().optional().default(true).describe("If true (default), update an existing socket with the same name; if false, error out instead."),
      dryRun: z.boolean().optional().default(false).describe("Validate without modifying the asset."),
    },
    async (args) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/add-skeleton-socket", args);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const verb = data.created ? "Created" : (data.updated ? "Updated" : "No-op");
      const tag = data.dryRun ? " (dry run)" : (data.saved ? "" : " [NOT SAVED]");
      return { content: [{ type: "text" as const, text: `${verb} socket '${data.socketName}' @ '${data.bone}' on ${data.skeleton}${tag}` }] };
    }
  );

  server.tool(
    "remove_skeleton_socket",
    "Remove a socket by name from a USkeleton asset. The skeleton is saved to disk. Wrapped in an undo transaction.",
    {
      path: z.string().describe("Package path of the USkeleton"),
      socketName: z.string().describe("Socket name to remove"),
      dryRun: z.boolean().optional().default(false),
    },
    async (args) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-skeleton-socket", args);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const tag = data.dryRun ? " (dry run)" : (data.saved ? "" : " [NOT SAVED]");
      return { content: [{ type: "text" as const, text: `Removed socket '${data.socketName}' from ${data.skeleton}${tag}` }] };
    }
  );

  server.tool(
    "copy_skeleton_sockets",
    "Copy all sockets from one USkeleton to another, preserving name, bone, and relative transform. Sockets whose target bone doesn't exist on the destination skeleton are skipped and reported under 'missingBones'. Use 'only' to restrict to a subset of socket names. Use 'overwrite=false' to skip sockets that already exist on the destination.",
    {
      fromPath: z.string().describe("Source USkeleton package path"),
      toPath: z.string().describe("Destination USkeleton package path"),
      only: z.array(z.string()).optional().describe("If provided, only copy sockets whose name is in this list (case-insensitive)."),
      overwrite: z.boolean().optional().default(true),
      dryRun: z.boolean().optional().default(false),
    },
    async (args) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/copy-skeleton-sockets", args);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      const tag = data.dryRun ? " (dry run)" : (data.saved ? "" : (data.created?.length || data.updated?.length ? " [NOT SAVED]" : ""));
      lines.push(`copy_skeleton_sockets: ${data.from} → ${data.to}${tag}`);
      if (data.created?.length) lines.push(`  Created (${data.created.length}): ${data.created.join(", ")}`);
      if (data.updated?.length) lines.push(`  Updated (${data.updated.length}): ${data.updated.join(", ")}`);
      if (data.skipped?.length) lines.push(`  Skipped existing (${data.skipped.length}): ${data.skipped.join(", ")}`);
      if (data.missingBones?.length) {
        lines.push(`  Missing target bones (${data.missingBones.length}):`);
        for (const m of data.missingBones) lines.push(`    ${m.socket} expected bone '${m.bone}'`);
      }
      if (!data.created?.length && !data.updated?.length && !data.skipped?.length && !data.missingBones?.length) {
        lines.push(`  (no sockets to copy)`);
      }
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
