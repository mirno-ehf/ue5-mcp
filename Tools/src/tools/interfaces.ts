import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerInterfaceTools(server: McpServer): void {
  server.tool(
    "list_interfaces",
    "List all Blueprint Interfaces implemented by a Blueprint. Shows interface name, class path, and function graphs for each.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-interfaces", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Interfaces implemented: ${data.count}`);

      if (data.interfaces?.length) {
        lines.push(``);
        for (const iface of data.interfaces) {
          lines.push(`  ${iface.name}`);
          lines.push(`    Class path: ${iface.classPath}`);
          if (iface.functions?.length) {
            lines.push(`    Functions: ${iface.functions.join(", ")}`);
          }
        }
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_interface",
    "Add a Blueprint Interface implementation to a Blueprint. The interface must be a Blueprint Interface asset (e.g. 'BPI_MyInterface') or a native UInterface class. Automatically creates function stub graphs for the interface's methods.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      interfaceName: z.string().describe("Interface name — Blueprint Interface asset name (e.g. 'BPI_MyInterface') or native UInterface class name"),
    },
    async ({ blueprint, interfaceName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/add-interface", { blueprint, interfaceName });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Interface added successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Interface: ${data.interfaceName}`);
      lines.push(`Interface path: ${data.interfacePath}`);
      if (data.functionGraphsAdded?.length) {
        lines.push(`Function stubs created: ${data.functionGraphsAdded.join(", ")}`);
      }
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_interfaces(blueprint="${blueprint}") — verify the interface was added`);
      lines.push(`  get_blueprint_graph(blueprint="${blueprint}", graph="<functionName>") — inspect a function stub`);
      lines.push(`  add_node(blueprint="${blueprint}", graph="<functionName>", ...) — add logic to a function stub`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "remove_interface",
    "Remove a Blueprint Interface implementation from a Blueprint. Optionally preserve the function graphs as regular functions.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      interfaceName: z.string().describe("Interface name to remove (e.g. 'BPI_MyInterface' or 'BPI_MyInterface_C')"),
      preserveFunctions: z.boolean().optional().describe("If true, keep the function graphs as regular Blueprint functions instead of deleting them (default: false)"),
    },
    async ({ blueprint, interfaceName, preserveFunctions }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, interfaceName };
      if (preserveFunctions !== undefined) body.preserveFunctions = preserveFunctions;

      const data = await uePost("/api/remove-interface", body);
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.implementedInterfaces?.length) {
          msg += `\nImplemented interfaces: ${data.implementedInterfaces.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Interface removed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Interface: ${data.interfaceName}`);
      lines.push(`Functions preserved: ${data.preservedFunctions}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_interfaces(blueprint="${blueprint}") — verify the interface was removed`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
