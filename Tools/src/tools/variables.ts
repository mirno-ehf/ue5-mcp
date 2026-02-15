import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS, formatUpdatedState } from "../helpers.js";

export function registerVariableTools(server: McpServer): void {
  server.tool(
    "change_variable_type",
    `Change a Blueprint member variable's type to a new struct or enum type. Compiles and saves the Blueprint. Downstream Make/Break nodes using the old type will need manual fixing. ${TYPE_NAME_DOCS} Pass dryRun=true to preview changes without saving.`,
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientManager')"),
      variable: z.string().describe("Variable name (e.g. 'Vitals')"),
      newType: z.string().describe("New type name with prefix (e.g. 'FVitals', 'ELungSound')"),
      typeCategory: z.enum(["struct", "enum"]).describe("Whether the new type is a struct or enum"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Blueprint"),
      batch: z.array(z.object({
        blueprint: z.string(),
        variable: z.string(),
        newType: z.string(),
        typeCategory: z.enum(["struct", "enum"]),
      })).optional().describe("Batch mode: array of {blueprint, variable, newType, typeCategory} objects. When provided, single params are ignored."),
    },
    async ({ blueprint, variable, newType, typeCategory, dryRun, batch }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = batch
        ? { batch }
        : { blueprint, variable, newType, typeCategory };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/change-variable-type", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);

      if (data.results) {
        // Batch response
        lines.push(`Batch variable type change: ${data.results.length} operation(s)`);
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED ${r.blueprint}.${r.variable}: ${r.error}`);
          } else {
            lines.push(`  OK ${r.blueprint}.${r.variable} \u2192 ${r.newType}`);
          }
        }
      } else {
        lines.push(`Variable type changed successfully.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        lines.push(`Variable: ${data.variable}`);
        lines.push(`New type: ${data.newType} (${data.typeCategory})`);
        lines.push(`Saved: ${data.saved}`);
      }

      // Updated state (#11)
      lines.push(...formatUpdatedState(data));

      // Tool chaining hints (#12)
      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Run refresh_all_nodes to update all nodes in the Blueprint`);
        lines.push(`  2. Check Break/Make struct nodes \u2014 they may need change_struct_node_type`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_variable",
    `Add a new member variable to a Blueprint. Supports simple types (bool, int, float, string, name, text, byte), built-in structs (vector, rotator, transform), and custom struct/enum types. ${TYPE_NAME_DOCS}`,
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      variableName: z.string().describe("Name for the new variable (e.g. 'Health', 'bIsActive')"),
      variableType: z.string().describe("Type: 'bool', 'int', 'float', 'string', 'name', 'text', 'byte', 'vector', 'rotator', 'transform', or struct/enum name (e.g. 'FVitals', 'EMyEnum')"),
      category: z.string().optional().describe("Variable category for organization in the Blueprint editor"),
      isArray: z.boolean().optional().describe("Create as an array variable (default: false)"),
      defaultValue: z.string().optional().describe("Default value as a string (e.g. 'true', '42', '0.5')"),
    },
    async ({ blueprint, variableName, variableType, category, isArray, defaultValue }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, variableName, variableType };
      if (category) body.category = category;
      if (isArray !== undefined) body.isArray = isArray;
      if (defaultValue !== undefined) body.defaultValue = defaultValue;

      const data = await uePost("/api/add-variable", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Variable added successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Variable: ${data.variableName}`);
      lines.push(`Type: ${data.variableType}${data.isArray ? " (Array)" : ""}`);
      if (data.category) lines.push(`Category: ${data.category}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  add_node(blueprint="${blueprint}", graph="EventGraph", nodeType="VariableGet", variableName="${variableName}") — read the variable`);
      lines.push(`  add_node(blueprint="${blueprint}", graph="EventGraph", nodeType="VariableSet", variableName="${variableName}") — write the variable`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "remove_variable",
    "Remove a member variable from a Blueprint. Also cleans up any VariableGet/VariableSet nodes referencing it.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      variableName: z.string().describe("Name of the variable to remove"),
    },
    async ({ blueprint, variableName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-variable", { blueprint, variableName });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableVariables?.length) {
          msg += `\nAvailable variables: ${data.availableVariables.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Variable removed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Variable: ${data.variableName}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_variable_metadata",
    "Set variable properties beyond type: category, tooltip, replication, exposeOnSpawn, editability, isPrivate. Provide any combination of fields to update.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      variable: z.string().describe("Variable name"),
      category: z.string().optional().describe("Variable category for organization in the editor"),
      tooltip: z.string().optional().describe("Tooltip text shown in the editor"),
      replication: z.enum(["none", "replicated", "repNotify"]).optional().describe("Replication mode"),
      exposeOnSpawn: z.boolean().optional().describe("Whether to expose the variable as a pin on SpawnActor"),
      editability: z.enum(["editAnywhere", "editDefaultsOnly", "editInstanceOnly", "none"]).optional()
        .describe("Edit visibility: editAnywhere (CDO + instances), editDefaultsOnly (CDO only), editInstanceOnly (instances only), none"),
      isPrivate: z.boolean().optional().describe("Mark variable as private (only accessible within this Blueprint)"),
    },
    async ({ blueprint, variable, category, tooltip, replication, exposeOnSpawn, editability, isPrivate }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, variable };
      if (category !== undefined) body.category = category;
      if (tooltip !== undefined) body.tooltip = tooltip;
      if (replication !== undefined) body.replication = replication;
      if (exposeOnSpawn !== undefined) body.exposeOnSpawn = exposeOnSpawn;
      if (editability !== undefined) body.editability = editability;
      if (isPrivate !== undefined) body.isPrivate = isPrivate;

      const data = await uePost("/api/set-variable-metadata", body);

      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableVariables?.length) {
          msg += `\nAvailable variables: ${data.availableVariables.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Variable metadata updated successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Variable: ${data.variable}`);

      if (data.changes?.length) {
        lines.push(`\nChanges:`);
        for (const c of data.changes) {
          if (c.oldValue !== undefined) {
            lines.push(`  ${c.field}: ${c.oldValue} -> ${c.newValue}`);
          } else {
            lines.push(`  ${c.field}: ${c.newValue}`);
          }
        }
      }

      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
