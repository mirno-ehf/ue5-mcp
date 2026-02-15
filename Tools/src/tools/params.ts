import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS, formatUpdatedState } from "../helpers.js";

export function registerParamTools(server: McpServer): void {
  server.tool(
    "change_function_parameter_type",
    `Change a function or custom event parameter's type to a new C++ struct type. Works with both Blueprint functions (K2Node_FunctionEntry) and custom events (K2Node_CustomEvent). Reconstructs the node to update output pins. Call refresh_all_nodes afterwards to propagate changes to downstream Break nodes. ${TYPE_NAME_DOCS} Pass dryRun=true to preview changes without saving.`,
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientManager')"),
      functionName: z.string().describe("Function or custom event name (e.g. 'UpdateVitals', 'SetSkinState')"),
      paramName: z.string().describe("Parameter name to change (e.g. 'Vitals', 'SkinState')"),
      newType: z.string().describe("New struct type name with F prefix (e.g. 'FVitals', 'FSkinState')"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Blueprint"),
      batch: z.array(z.object({
        blueprint: z.string(),
        functionName: z.string(),
        paramName: z.string(),
        newType: z.string(),
      })).optional().describe("Batch mode: array of {blueprint, functionName, paramName, newType} objects. When provided, single params are ignored."),
    },
    async ({ blueprint, functionName, paramName, newType, dryRun, batch }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = batch
        ? { batch }
        : { blueprint, functionName, paramName, newType };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/change-function-param-type", body);

      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableParams) {
          msg += `\nAvailable parameters: ${data.availableParams.join(", ")}`;
        }
        if (data.availableFunctionsAndEvents) {
          msg += `\nAvailable functions/events: ${data.availableFunctionsAndEvents.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);

      if (data.results) {
        // Batch response
        lines.push(`Batch parameter type change: ${data.results.length} operation(s)`);
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED ${r.blueprint}.${r.functionName}.${r.paramName}: ${r.error}`);
          } else {
            lines.push(`  OK ${r.blueprint}.${r.functionName}.${r.paramName} \u2192 ${r.newType}`);
          }
        }
      } else {
        lines.push(`Parameter type changed successfully.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        lines.push(`${data.nodeType}: ${data.functionName}`);
        lines.push(`Parameter: ${data.paramName} \u2192 ${data.newType}`);
        lines.push(`Node ID: ${data.nodeId}`);
        lines.push(`Saved: ${data.saved}`);
      }

      // Updated state (#11)
      lines.push(...formatUpdatedState(data));

      // Tool chaining hints (#12)
      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Check delegate graphs that bind to this function/event`);
        lines.push(`  2. Run refresh_all_nodes to propagate pin changes downstream`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "remove_function_parameter",
    "Remove a parameter from a Blueprint function, custom event, or event dispatcher delegate. Works by finding the FunctionEntry/CustomEvent node in the function/delegate signature graph and removing the UserDefinedPin. Reconstructs the node and saves. Use this to remove delegate parameters that reference deleted types.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BPC_DeviceController')"),
      functionName: z.string().describe("Function, custom event, or event dispatcher name (e.g. 'OnDeviceStateChanged')"),
      paramName: z.string().describe("Parameter name to remove (e.g. 'DeviceState')"),
    },
    async ({ blueprint, functionName, paramName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-function-parameter", {
        blueprint, functionName, paramName,
      });

      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableParams) {
          msg += `\nAvailable parameters: ${data.availableParams.join(", ")}`;
        }
        if (data.availableFunctionsAndEvents) {
          msg += `\nAvailable functions/events: ${data.availableFunctionsAndEvents.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Parameter removed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`${data.nodeType}: ${data.functionName}`);
      lines.push(`Removed parameter: ${data.paramName}`);
      lines.push(`Node ID: ${data.nodeId}`);
      lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_function_parameter",
    `Add a typed parameter to an existing function, custom event, or event dispatcher signature. Works with all three — specify the function/event/dispatcher name in functionName. ${TYPE_NAME_DOCS}`,
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      functionName: z.string().describe("Name of the function, custom event, or event dispatcher to add the parameter to"),
      paramName: z.string().describe("Name for the new parameter"),
      paramType: z.string().describe("Type for the new parameter (e.g. 'float', 'bool', 'string', 'FVector', 'object')"),
    },
    async ({ blueprint, functionName, paramName, paramType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/add-function-parameter", {
        blueprint, functionName, paramName, paramType,
      });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableFunctions?.length) {
          msg += `\nAvailable functions/events/dispatchers:\n  ${data.availableFunctions.join("\n  ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Parameter added successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Function: ${data.functionName} (${data.nodeType})`);
      lines.push(`Parameter: ${data.paramName}: ${data.paramType}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  get_blueprint_graph(blueprint="${blueprint}", graph="${functionName}") — inspect the updated signature`);
      lines.push(`  add_function_parameter(blueprint="${blueprint}", functionName="${functionName}", ...) — add another parameter`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
