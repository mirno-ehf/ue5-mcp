import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS, flagType } from "../helpers.js";

export function registerSnapshotTools(server: McpServer): void {
  server.tool(
    "snapshot_graph",
    "Create a backup snapshot of a Blueprint graph's state (all nodes, pins, and connections). Use BEFORE any destructive operation (C++ rebuild, change_struct_node_type, bulk edits). Returns a snapshot ID for later use with diff_graph or restore_graph. Snapshots are stored server-side and persist to disk.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      graph: z.string().optional().describe("Specific graph name. If omitted, snapshots ALL graphs in the Blueprint"),
    },
    async (params) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

      const data = await uePost("/api/snapshot-graph", {
        blueprint: params.blueprint,
        graph: params.graph,
      });

      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [];
      lines.push(`Snapshot created: ${data.snapshotId}`);
      lines.push(`Blueprint: ${data.blueprint}`);
      if (data.graphs) {
        const graphNames = data.graphs.map((g: any) => `${g.name} (${g.nodeCount} nodes, ${g.connectionCount} connections)`);
        lines.push(`Graphs: ${graphNames.join(", ")}`);
      }
      lines.push(`Total connections captured: ${data.totalConnections}`);
      lines.push(``);
      lines.push(`**Next steps:**`);
      lines.push(`1. Make your changes (C++ rebuild, change_struct_node_type, etc.)`);
      lines.push(`2. Run **diff_graph** to see what changed`);
      lines.push(`3. Run **restore_graph** to reconnect any severed pins`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "diff_graph",
    "Compare current Blueprint graph state against a snapshot. Shows severed connections, new connections, type changes, and missing nodes. Use AFTER a potentially destructive operation to assess damage before restoring.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      snapshotId: z.string().describe("Snapshot ID from snapshot_graph"),
      graph: z.string().optional().describe("Specific graph to diff. If omitted, diffs all graphs in the snapshot"),
    },
    async (params) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

      const data = await uePost("/api/diff-graph", {
        blueprint: params.blueprint,
        snapshotId: params.snapshotId,
        graph: params.graph,
      });

      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [];
      lines.push(`# Diff: ${data.blueprint} vs ${data.snapshotId}`);

      // Per-graph diffs
      if (data.graphDiffs) {
        for (const gd of data.graphDiffs) {
          lines.push(``);
          lines.push(`## ${gd.graphName}`);

          if (gd.severedConnections?.length) {
            lines.push(`  Severed connections (${gd.severedConnections.length}):`);
            for (const sc of gd.severedConnections) {
              lines.push(`    ${sc.sourceNodeTitle}.${sc.sourcePinName} was -> ${sc.targetNodeTitle}.${sc.targetPinName}`);
            }
          }

          if (gd.newConnections?.length) {
            lines.push(`  New connections (${gd.newConnections.length}):`);
            for (const nc of gd.newConnections) {
              lines.push(`    ${nc.sourceNodeTitle}.${nc.sourcePinName} -> ${nc.targetNodeTitle}.${nc.targetPinName}`);
            }
          }

          if (gd.typeChanges?.length) {
            lines.push(`  Type changes (${gd.typeChanges.length}):`);
            for (const tc of gd.typeChanges) {
              lines.push(`    Node ${tc.nodeId} (${tc.nodeTitle}): ${tc.oldType} -> ${tc.newType}`);
            }
          }

          if (gd.missingNodes?.length) {
            lines.push(`  Missing nodes (${gd.missingNodes.length}):`);
            for (const mn of gd.missingNodes) {
              lines.push(`    ${mn.nodeId} (${mn.nodeTitle}) - was ${mn.nodeClass}`);
            }
          }

          if (!gd.severedConnections?.length && !gd.newConnections?.length && !gd.typeChanges?.length && !gd.missingNodes?.length) {
            lines.push(`  No changes detected.`);
          }
        }
      }

      // Summary
      lines.push(``);
      if (data.summary) {
        lines.push(`Summary: ${data.summary.severed} severed, ${data.summary.new} new, ${data.summary.typeChanges} type changes, ${data.summary.missingNodes} missing nodes`);
      }

      // Next steps based on what was found
      lines.push(``);
      lines.push(`**Next steps:**`);
      if (data.summary?.typeChanges > 0) {
        lines.push(`1. Fix type changes first with **change_struct_node_type**`);
        lines.push(`2. Then run **restore_graph** to reconnect severed pins`);
      } else if (data.summary?.severed > 0) {
        lines.push(`1. Run **restore_graph** to reconnect severed pins`);
      } else {
        lines.push(`1. No action needed — graph is intact`);
      }
      lines.push(`3. Run **validate_blueprint** to verify clean compilation`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "restore_graph",
    "Reconnect severed pin connections from a snapshot. Use after diff_graph shows damage. Can restore an entire graph, a single node (nodeId), or use an explicit pin map. For Break/Make struct nodes that lost connections after change_struct_node_type or C++ rebuild, this bulk-reconnects all pins in one call instead of individual connect_pins calls.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      snapshotId: z.string().describe("Snapshot ID from snapshot_graph"),
      graph: z.string().optional().describe("Specific graph to restore. If omitted, restores all graphs"),
      nodeId: z.string().optional().describe("Scope restore to a single node (e.g. a Break struct node). Useful after change_struct_node_type"),
      pinMap: z.record(z.string(), z.object({
        targetNodeId: z.string(),
        targetPinName: z.string(),
      })).optional().describe("Explicit pin mapping override: {outputPinName: {targetNodeId, targetPinName}}. Use when no snapshot exists or snapshot is stale"),
      dryRun: z.boolean().optional().describe("Preview reconnections without making changes"),
    },
    async (params) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

      const data = await uePost("/api/restore-graph", {
        blueprint: params.blueprint,
        snapshotId: params.snapshotId,
        graph: params.graph,
        nodeId: params.nodeId,
        pinMap: params.pinMap,
        dryRun: params.dryRun ?? false,
      });

      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [];
      if (params.dryRun) {
        lines.push(`[DRY RUN - no changes will be made]\n`);
      }

      lines.push(`Restore: ${data.blueprint} from ${data.snapshotId}`);
      if (params.nodeId) {
        lines.push(`Scoped to node: ${params.nodeId}`);
      }
      lines.push(``);
      lines.push(`Reconnected: ${data.reconnected}/${data.reconnected + data.failed}`);
      if (data.failed > 0) {
        lines.push(`Failed: ${data.failed}`);
      }

      if (data.details?.length) {
        lines.push(``);
        lines.push(`Details:`);
        for (const d of data.details) {
          const status = d.result === "ok" ? "OK" : "FAILED";
          const reason = d.reason ? ` (${d.reason})` : "";
          lines.push(`  ${status}: ${d.sourcePinName} -> ${d.targetNodeTitle}.${d.targetPinName}${reason}`);
        }
      }

      if (!params.dryRun && data.saved !== undefined) {
        lines.push(``);
        lines.push(`Saved: ${data.saved}`);
      }

      lines.push(``);
      lines.push(`**Next steps:**`);
      if (data.failed > 0) {
        lines.push(`1. Fix ${data.failed} failed reconnection(s) manually with **connect_pins**`);
      }
      if (params.dryRun) {
        lines.push(`1. Re-run **restore_graph** without dryRun to apply changes`);
      }
      lines.push(`2. Run **validate_blueprint** to verify clean compilation`);
      lines.push(`3. Run **find_disconnected_pins** to verify no pins were missed`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "find_disconnected_pins",
    "Scan Blueprint(s) for pins that should be connected but aren't. Detects Break/Make struct nodes with broken types (HIGH confidence) or zero connections (MEDIUM confidence). Use after C++ rebuilds, change_struct_node_type, or refresh_all_nodes. Catches silent data flow breaks that validate_blueprint misses. Provide at least one of: blueprint, filter, or snapshotId.",
    {
      blueprint: z.string().optional().describe("Blueprint name or path. If omitted, scans multiple BPs using filter"),
      filter: z.string().optional().describe("Path filter when scanning multiple BPs (e.g. '/Game/Blueprints/Patients/'). Ignored when blueprint is specified"),
      snapshotId: z.string().optional().describe("Compare against snapshot for definite break detection. Without this, uses heuristics only"),
      sensitivity: z.enum(["high", "medium", "all"]).optional().describe("Detection sensitivity: 'high' = only broken types, 'medium' (default) = broken types + zero-connection Break nodes, 'all' = include partially connected Break nodes"),
    },
    async (params) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

      const data = await uePost("/api/find-disconnected-pins", {
        blueprint: params.blueprint,
        filter: params.filter,
        snapshotId: params.snapshotId,
        sensitivity: params.sensitivity ?? "medium",
      });

      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [];

      if (!data.results || data.results.length === 0) {
        lines.push(`No disconnected pins found.`);
        if (data.summary) {
          lines.push(`Scanned ${data.summary.blueprintsScanned} Blueprint(s) — all Break/Make struct nodes have valid types and connected outputs.`);
        }
        return { content: [{ type: "text" as const, text: lines.join("\n") }] };
      }

      // Group by blueprint
      const byBP: Record<string, any[]> = {};
      for (const r of data.results) {
        const bp = r.blueprint || params.blueprint || "unknown";
        if (!byBP[bp]) byBP[bp] = [];
        byBP[bp].push(r);
      }

      lines.push(`Disconnected pins found:\n`);

      for (const [bp, results] of Object.entries(byBP)) {
        lines.push(`## ${bp}`);
        for (const r of results) {
          const conf = r.confidence === "high" ? "HIGH" : r.confidence === "medium" ? "MEDIUM" : "LOW";
          const icon = r.confidence === "high" ? "\u26A0" : r.confidence === "medium" ? "\u26A1" : "\u2139";
          lines.push(`  ${icon} ${conf} — ${r.nodeTitle || "BreakStruct"} (${r.nodeId}) in ${r.graph}`);
          lines.push(`    Type: ${flagType(r.structType || "")}`);
          lines.push(`    Reason: ${r.reason}`);
          if (r.pins?.length) {
            for (const p of r.pins) {
              const was = p.wasConnectedTo ? ` (was -> ${p.wasConnectedTo})` : "";
              lines.push(`      ${p.name}: ${p.type} — disconnected${was}`);
            }
          }
        }
        lines.push(``);
      }

      if (data.summary) {
        lines.push(`Summary: ${data.summary.high} HIGH, ${data.summary.medium} MEDIUM across ${data.summary.blueprintsScanned} Blueprint(s)`);
      }

      lines.push(``);
      lines.push(`**Next steps:**`);
      if (data.summary?.high > 0) {
        lines.push(`1. Fix HIGH-confidence issues: use **change_struct_node_type** to restore struct types`);
      }
      lines.push(`2. Use **restore_graph** (if snapshot exists) to bulk-reconnect severed pins`);
      lines.push(`3. Or use **connect_pins** for individual reconnections`);
      lines.push(`4. Run **validate_blueprint** to verify compilation`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "analyze_rebuild_impact",
    "Predict which Blueprints will be affected by a C++ module rebuild. Scans for Break/Make struct nodes, variables, and function parameters that reference USTRUCTs/UENUMs defined in the specified module. Use BEFORE rebuilding to know what to snapshot. " + TYPE_NAME_DOCS,
    {
      moduleName: z.string().describe("C++ module name (e.g. 'MyGame'). Finds all Blueprints using types from this module"),
      structNames: z.array(z.string()).optional().describe("Specific struct/enum names to check (e.g. ['FVitals', 'FSkinState']). If omitted, checks ALL types from the module"),
    },
    async (params) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

      const data = await uePost("/api/analyze-rebuild-impact", {
        moduleName: params.moduleName,
        structNames: params.structNames,
      });

      if (data.error) {
        return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      }

      const lines: string[] = [];
      lines.push(`# Rebuild Impact Analysis: ${data.moduleName}`);
      lines.push(``);

      if (data.typesFound?.length) {
        lines.push(`Types in module (${data.typesFound.length}): ${data.typesFound.join(", ")}`);
        lines.push(``);
      }

      if (!data.affectedBlueprints?.length) {
        lines.push(`No Blueprints reference types from this module.`);
        return { content: [{ type: "text" as const, text: lines.join("\n") }] };
      }

      lines.push(`Affected Blueprints (${data.affectedBlueprints.length}):\n`);

      for (const bp of data.affectedBlueprints) {
        const risk = bp.risk || "UNKNOWN";
        lines.push(`  **${bp.name}** (${risk} risk)`);
        if (bp.breakNodes > 0) lines.push(`    Break nodes: ${bp.breakNodes}${bp.breakNodeTypes ? ` (${bp.breakNodeTypes.join(", ")})` : ""}`);
        if (bp.makeNodes > 0) lines.push(`    Make nodes: ${bp.makeNodes}${bp.makeNodeTypes ? ` (${bp.makeNodeTypes.join(", ")})` : ""}`);
        if (bp.variables > 0) lines.push(`    Variables: ${bp.variables}`);
        if (bp.functionParams > 0) lines.push(`    Function params: ${bp.functionParams}`);
        if (bp.connectionsAtRisk > 0) lines.push(`    Connections at risk: ~${bp.connectionsAtRisk}`);
        lines.push(``);
      }

      if (data.summary) {
        lines.push(`Total: ${data.summary.totalBlueprints} Blueprints, ${data.summary.totalBreakMakeNodes} Break/Make nodes, ~${data.summary.totalConnectionsAtRisk} connections at risk`);
      }

      lines.push(``);
      lines.push(`**Next steps:**`);
      lines.push(`1. Run **snapshot_graph** on each HIGH-risk Blueprint BEFORE rebuilding:`);
      const highRisk = (data.affectedBlueprints || []).filter((bp: any) => bp.risk === "HIGH");
      for (const bp of highRisk.slice(0, 5)) {
        lines.push(`   snapshot_graph(blueprint="${bp.name}")`);
      }
      if (highRisk.length > 5) {
        lines.push(`   ... and ${highRisk.length - 5} more`);
      }
      lines.push(`2. After rebuild, run **find_disconnected_pins** to assess damage`);
      lines.push(`3. Use **restore_graph** on each Blueprint to reconnect severed pins`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
