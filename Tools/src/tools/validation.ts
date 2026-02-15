import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerValidationTools(server: McpServer): void {
  server.tool(
    "validate_blueprint",
    "Compile a Blueprint and report errors/warnings without saving. Captures both node-level compiler messages AND log-level messages (e.g. 'Can\\'t connect pins', 'Fixed up function'). Use after making changes to verify correctness.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientManager')"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/validate-blueprint", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Validation: ${data.blueprint}`);
      lines.push(`Status: ${data.status || (data.errors?.length ? "ERRORS" : "OK")}`);
      lines.push(`Valid: ${data.isValid}`);

      if (data.compileWarning) {
        lines.push(`\n\u26A0 ${data.compileWarning}`);
      }

      if (data.errors?.length) {
        lines.push(`\nErrors (${data.errorCount}):`);
        for (const e of data.errors) {
          if (typeof e === "string") {
            lines.push(`  \u2716 ${e}`);
          } else {
            const src = e.source === "log" ? "[log] " : "";
            const loc = e.graph ? `[${e.graph}] ` : "";
            const node = e.nodeTitle ? `${e.nodeTitle}: ` : "";
            lines.push(`  \u2716 ${src}${loc}${node}${e.message}`);
          }
        }
      }

      if (data.warnings?.length) {
        lines.push(`\nWarnings (${data.warningCount}):`);
        for (const w of data.warnings) {
          if (typeof w === "string") {
            lines.push(`  \u26A0 ${w}`);
          } else {
            const src = w.source === "log" ? "[log] " : "";
            const loc = w.graph ? `[${w.graph}] ` : "";
            const node = w.nodeTitle ? `${w.nodeTitle}: ` : "";
            lines.push(`  \u26A0 ${src}${loc}${node}${w.message}`);
          }
        }
      }

      if (!data.errors?.length && !data.warnings?.length) {
        lines.push(`\nNo errors or warnings. Blueprint compiles cleanly.`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "validate_all_blueprints",
    "Bulk-validate all Blueprints (or a filtered subset) by compiling each one and reporting errors. Use after reparenting, C++ changes, or any operation that could cause cascading breakage. Returns only failed Blueprints to keep output manageable. Sends progress notifications during validation.",
    {
      filter: z.string().optional().describe("Optional path or name filter (e.g. '/Game/Blueprints/WebUI/' or 'HUD'). If omitted, validates ALL blueprints."),
      batchSize: z.number().optional().describe("Number of blueprints to validate per batch (default 50). Smaller batches give more frequent progress updates."),
    },
    async ({ filter, batchSize: batchSizeParam }, extra) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const batchSize = batchSizeParam ?? 50;

      // Step 1: Get total count (fast, no compilation)
      const countBody: Record<string, unknown> = { countOnly: true };
      if (filter) countBody.filter = filter;

      const countData = await uePost("/api/validate-all-blueprints", countBody);
      if (countData.error) return { content: [{ type: "text" as const, text: `Error: ${countData.error}` }] };

      const totalMatching: number = countData.totalMatching ?? 0;

      if (totalMatching === 0) {
        const lines = ["# Bulk Validation Results"];
        if (filter) lines.push(`Filter: ${filter}`);
        lines.push(`No matching blueprints found.`);
        return { content: [{ type: "text" as const, text: lines.join("\n") }] };
      }

      // Extract progress token from MCP request metadata
      const progressToken = extra._meta?.progressToken;

      // Step 2: Iterate in batches
      let totalChecked = 0;
      let totalPassed = 0;
      let totalFailed = 0;
      let totalCrashed = 0;
      const allFailed: any[] = [];

      for (let offset = 0; offset < totalMatching; offset += batchSize) {
        const body: Record<string, unknown> = { offset, limit: batchSize };
        if (filter) body.filter = filter;

        const data = await uePost("/api/validate-all-blueprints", body);
        if (data.error) return { content: [{ type: "text" as const, text: `Error at offset ${offset}: ${data.error}` }] };

        totalChecked += data.totalChecked ?? 0;
        totalPassed += data.totalPassed ?? 0;
        totalFailed += data.totalFailed ?? 0;
        totalCrashed += (data.totalCrashed ?? 0);

        if (data.failed?.length) {
          allFailed.push(...data.failed);
        }

        // Send MCP progress notification if client requested it
        if (progressToken !== undefined) {
          try {
            await extra.sendNotification({
              method: "notifications/progress",
              params: {
                progressToken,
                progress: Math.min(offset + batchSize, totalMatching),
                total: totalMatching,
                message: `Validated ${totalChecked}/${totalMatching} blueprints (${totalFailed} failed)`,
              },
            });
          } catch {
            // Progress notifications are best-effort per MCP spec
          }
        }
      }

      // Step 3: Format aggregated results
      const lines: string[] = [];
      lines.push(`# Bulk Validation Results`);
      if (filter) lines.push(`Filter: ${filter}`);
      lines.push(`Checked: ${totalChecked}`);
      lines.push(`Passed: ${totalPassed}`);
      lines.push(`Failed: ${totalFailed}`);
      if (totalCrashed) lines.push(`Crashed: ${totalCrashed}`);

      if (allFailed.length) {
        lines.push(`\n## Failed Blueprints\n`);
        for (const bp of allFailed) {
          lines.push(`### ${bp.blueprint} (${bp.path || ""})`);
          lines.push(`Status: ${bp.status} | Errors: ${bp.errorCount} | Warnings: ${bp.warningCount}`);

          if (bp.errors?.length) {
            for (const e of bp.errors) {
              if (typeof e === "string") {
                lines.push(`  \u2716 ${e}`);
              } else {
                const src = e.source === "log" ? "[log] " : "";
                const loc = e.graph ? `[${e.graph}] ` : "";
                const node = e.nodeTitle ? `${e.nodeTitle}: ` : "";
                lines.push(`  \u2716 ${src}${loc}${node}${e.message}`);
              }
            }
          }

          if (bp.warnings?.length) {
            for (const w of bp.warnings) {
              if (typeof w === "string") {
                lines.push(`  \u26A0 ${w}`);
              } else {
                const src = w.source === "log" ? "[log] " : "";
                const loc = w.graph ? `[${w.graph}] ` : "";
                const node = w.nodeTitle ? `${w.nodeTitle}: ` : "";
                lines.push(`  \u26A0 ${src}${loc}${node}${w.message}`);
              }
            }
          }
          lines.push("");
        }
      } else {
        lines.push(`\nAll blueprints compile cleanly!`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
