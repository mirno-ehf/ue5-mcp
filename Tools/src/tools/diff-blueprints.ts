import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerDiffBlueprintsTools(server: McpServer): void {
  server.tool(
    "diff_blueprints",
    "Structural diff between two different Blueprints. Compares nodes, connections, and variables across graphs. Use for comparing patient variants, finding divergence after copy-paste, or auditing consistency.",
    {
      blueprintA: z.string().describe("First Blueprint name or package path"),
      blueprintB: z.string().describe("Second Blueprint name or package path"),
      graph: z.string().optional().describe("Specific graph to compare (e.g. 'EventGraph'). If omitted, compares all graphs."),
    },
    async ({ blueprintA, blueprintB, graph }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprintA, blueprintB };
      if (graph) body.graph = graph;

      const data = await uePost("/api/diff-blueprints", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Diff: ${data.blueprintA} vs ${data.blueprintB}`);
      lines.push(`Total differences: ${data.totalDifferences}\n`);

      for (const g of data.graphs || []) {
        const status = g.status === "identical" ? "IDENTICAL" :
                       g.status === "onlyInA" ? `ONLY IN A (${g.nodeCountA} nodes)` :
                       g.status === "onlyInB" ? `ONLY IN B (${g.nodeCountB} nodes)` :
                       "DIFFERENT";
        lines.push(`--- ${g.graph}: ${status} ---`);

        if (g.nodeCountA !== undefined && g.nodeCountB !== undefined) {
          lines.push(`  Nodes: A=${g.nodeCountA}, B=${g.nodeCountB}`);
        }

        if (g.nodesOnlyInA?.length) {
          lines.push(`  Nodes only in A:`);
          for (const n of g.nodesOnlyInA) {
            lines.push(`    - ${n.title} (${n.class}) x${n.extraCount}`);
          }
        }
        if (g.nodesOnlyInB?.length) {
          lines.push(`  Nodes only in B:`);
          for (const n of g.nodesOnlyInB) {
            lines.push(`    - ${n.title} (${n.class}) x${n.extraCount}`);
          }
        }
        if (g.connectionsOnlyInA?.length) {
          lines.push(`  Connections only in A: ${g.connectionsOnlyInA.length}`);
          for (const c of g.connectionsOnlyInA.slice(0, 10)) {
            lines.push(`    - ${c}`);
          }
          if (g.connectionsOnlyInA.length > 10) lines.push(`    ... and ${g.connectionsOnlyInA.length - 10} more`);
        }
        if (g.connectionsOnlyInB?.length) {
          lines.push(`  Connections only in B: ${g.connectionsOnlyInB.length}`);
          for (const c of g.connectionsOnlyInB.slice(0, 10)) {
            lines.push(`    - ${c}`);
          }
          if (g.connectionsOnlyInB.length > 10) lines.push(`    ... and ${g.connectionsOnlyInB.length - 10} more`);
        }
        lines.push(``);
      }

      if (data.variablesOnlyInA?.length) {
        lines.push(`Variables only in A: ${data.variablesOnlyInA.join(", ")}`);
      }
      if (data.variablesOnlyInB?.length) {
        lines.push(`Variables only in B: ${data.variablesOnlyInB.join(", ")}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
