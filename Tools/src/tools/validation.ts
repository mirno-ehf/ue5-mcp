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
    "Bulk-validate all Blueprints (or a filtered subset) by compiling each one and reporting errors. Use after reparenting, C++ changes, or any operation that could cause cascading breakage. Returns only failed Blueprints to keep output manageable. Can take several minutes for large projects.",
    {
      filter: z.string().optional().describe("Optional path or name filter (e.g. '/Game/Blueprints/WebUI/' or 'HUD'). If omitted, validates ALL blueprints."),
    },
    async ({ filter }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, string> = {};
      if (filter) body.filter = filter;

      const data = await uePost("/api/validate-all-blueprints", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`# Bulk Validation Results`);
      if (data.filter) lines.push(`Filter: ${data.filter}`);
      lines.push(`Checked: ${data.totalChecked}`);
      lines.push(`Passed: ${data.totalPassed}`);
      lines.push(`Failed: ${data.totalFailed}`);
      if (data.totalCrashed) lines.push(`Crashed: ${data.totalCrashed}`);

      if (data.failed?.length) {
        lines.push(`\n## Failed Blueprints\n`);
        for (const bp of data.failed) {
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
