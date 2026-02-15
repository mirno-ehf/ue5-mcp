import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS } from "../helpers.js";

export function registerUserTypeTools(server: McpServer): void {
  server.tool(
    "create_struct",
    `Create a new UserDefinedStruct asset. Optionally provide initial properties.\n\nType names for properties:\n${TYPE_NAME_DOCS}`,
    {
      assetPath: z.string().describe("Full asset path (e.g. '/Game/DataTypes/S_MyStruct')"),
      properties: z.array(z.object({
        name: z.string().describe("Property name"),
        type: z.string().describe("Property type (e.g. 'bool', 'int', 'float', 'string', 'FVector', 'FRotator')"),
      })).optional().describe("Initial properties to add"),
    },
    async ({ assetPath, properties }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { assetPath };
      if (properties) body.properties = properties;

      const data = await uePost("/api/create-struct", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Struct created successfully.`);
      lines.push(`Asset: ${data.assetPath}`);
      lines.push(`Properties added: ${data.propertiesAdded}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  add_struct_property — add more properties`);
      lines.push(`  search_by_type — find usages of this struct`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "create_enum",
    "Create a new UserDefinedEnum asset with the given values.",
    {
      assetPath: z.string().describe("Full asset path (e.g. '/Game/DataTypes/E_MyEnum')"),
      values: z.array(z.string()).describe("Enum value display names (e.g. ['Low', 'Medium', 'High'])"),
    },
    async ({ assetPath, values }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/create-enum", { assetPath, values });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Enum created successfully.`);
      lines.push(`Asset: ${data.assetPath}`);
      lines.push(`Values: ${data.valueCount}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_struct_property",
    `Add a property to an existing UserDefinedStruct.\n\nType names:\n${TYPE_NAME_DOCS}`,
    {
      assetPath: z.string().describe("Struct asset path (e.g. '/Game/DataTypes/S_MyStruct')"),
      name: z.string().describe("Property name"),
      type: z.string().describe("Property type (e.g. 'bool', 'int', 'float', 'string', 'FVector')"),
    },
    async ({ assetPath, name, type }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/add-struct-property", { assetPath, name, type });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Property added successfully.`);
      lines.push(`Struct: ${data.assetPath}`);
      lines.push(`Property: ${data.propertyName}: ${data.propertyType}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "remove_struct_property",
    "Remove a property from an existing UserDefinedStruct.",
    {
      assetPath: z.string().describe("Struct asset path (e.g. '/Game/DataTypes/S_MyStruct')"),
      name: z.string().describe("Property name to remove"),
    },
    async ({ assetPath, name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-struct-property", { assetPath, name });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availableProperties?.length) {
          msg += `\nAvailable properties: ${data.availableProperties.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Property removed successfully.`);
      lines.push(`Struct: ${data.assetPath}`);
      lines.push(`Removed: ${data.removedProperty}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
