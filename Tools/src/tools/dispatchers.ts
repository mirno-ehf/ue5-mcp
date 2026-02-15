import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS } from "../helpers.js";

export function registerDispatcherTools(server: McpServer): void {
  server.tool(
    "add_event_dispatcher",
    `Create an event dispatcher (multicast delegate) on a Blueprint. Optionally include typed parameters in the dispatcher signature. ${TYPE_NAME_DOCS}`,
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      dispatcherName: z.string().describe("Name for the event dispatcher (e.g. 'OnHealthChanged')"),
      parameters: z.array(z.object({
        name: z.string().describe("Parameter name"),
        type: z.string().describe("Parameter type (e.g. 'float', 'bool', 'string', 'FVector', 'object')"),
      })).optional().describe("Optional array of typed parameters for the dispatcher signature"),
    },
    async ({ blueprint, dispatcherName, parameters }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, dispatcherName };
      if (parameters?.length) body.parameters = parameters;

      const data = await uePost("/api/add-event-dispatcher", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Event dispatcher created successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Dispatcher: ${data.dispatcherName}`);
      if (data.parameters?.length) {
        lines.push(`Parameters:`);
        for (const p of data.parameters) {
          lines.push(`  ${p.name}: ${p.type}`);
        }
      } else {
        lines.push(`Parameters: (none)`);
      }
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_event_dispatchers(blueprint="${blueprint}") — verify the dispatcher was created`);
      lines.push(`  add_function_parameter(blueprint="${blueprint}", functionName="${dispatcherName}", ...) — add more parameters`);
      lines.push(`  add_node(blueprint="${blueprint}", graph="EventGraph", nodeType="CallFunction", functionName="<dispatcherName>_Event") — bind to it`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_event_dispatchers",
    "List all event dispatchers (multicast delegates) on a Blueprint, including their parameter signatures.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-event-dispatchers", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Event dispatchers: ${data.count}`);

      if (data.dispatchers?.length) {
        lines.push(``);
        for (const d of data.dispatchers) {
          if (d.parameters?.length) {
            const paramStr = d.parameters.map((p: any) => `${p.name}: ${p.type}`).join(", ");
            lines.push(`  ${d.name}(${paramStr})`);
          } else {
            lines.push(`  ${d.name}()`);
          }
        }
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
