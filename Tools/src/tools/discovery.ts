import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerDiscoveryTools(server: McpServer): void {
  server.tool(
    "get_pin_info",
    "Get detailed information about a specific pin on a Blueprint node, including type details, container type (array/set/map), default value, and current connections.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().describe("Node GUID"),
      pinName: z.string().describe("Pin name"),
    },
    async ({ blueprint, nodeId, pinName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/get-pin-info", { blueprint, nodeId, pinName });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availablePins?.length) {
          msg += `\n\nAvailable pins:`;
          for (const p of data.availablePins) {
            msg += `\n  ${p.direction === "Output" ? "\u2192" : "\u2190"} ${p.name}: ${p.type}`;
          }
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Pin: ${data.pinName}`);
      lines.push(`Direction: ${data.direction}`);
      lines.push(`Type: ${data.type}${data.subtype ? ` (${data.subtype})` : ""}${data.subCategory ? ` [${data.subCategory}]` : ""}`);

      const containers: string[] = [];
      if (data.isArray) containers.push("Array");
      if (data.isSet) containers.push("Set");
      if (data.isMap) containers.push("Map");
      if (containers.length) lines.push(`Container: ${containers.join(", ")}`);

      if (data.isReference) lines.push(`Reference: true`);
      if (data.isConst) lines.push(`Const: true`);

      if (data.defaultValue !== undefined) lines.push(`Default value: ${data.defaultValue}`);
      if (data.defaultTextValue !== undefined) lines.push(`Default text: ${data.defaultTextValue}`);
      if (data.defaultObject !== undefined) lines.push(`Default object: ${data.defaultObject}`);

      if (data.connectedTo?.length) {
        lines.push(`\nConnections (${data.connectedTo.length}):`);
        for (const c of data.connectedTo) {
          lines.push(`  ${c.nodeTitle} (${c.nodeId}).${c.pinName}`);
        }
      } else {
        lines.push(`\nConnections: none`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "check_pin_compatibility",
    "Check whether two pins can be connected before attempting connect_pins. Returns compatibility status, connection type (direct, requires conversion, etc.), and any UE5 schema messages.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      sourceNodeId: z.string().describe("Source node GUID"),
      sourcePinName: z.string().describe("Source pin name"),
      targetNodeId: z.string().describe("Target node GUID"),
      targetPinName: z.string().describe("Target pin name"),
    },
    async ({ blueprint, sourceNodeId, sourcePinName, targetNodeId, targetPinName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/check-pin-compatibility", {
        blueprint, sourceNodeId, sourcePinName, targetNodeId, targetPinName,
      });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      const icon = data.compatible ? "\u2705" : "\u274c";
      lines.push(`${icon} Compatible: ${data.compatible}`);
      lines.push(`Connection type: ${data.connectionType}`);
      if (data.message) lines.push(`Message: ${data.message}`);
      lines.push(``);
      lines.push(`Source pin type: ${data.sourcePinType}${data.sourcePinSubtype ? ` (${data.sourcePinSubtype})` : ""}`);
      lines.push(`Target pin type: ${data.targetPinType}${data.targetPinSubtype ? ` (${data.targetPinSubtype})` : ""}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_classes",
    "List available UE5 classes. Filter by name substring and/or parent class. Useful for discovering class names to use with add_node(CallFunction), add_node(DynamicCast), add_node(SpawnActorFromClass), etc.",
    {
      filter: z.string().optional().describe("Substring to match against class name (case-insensitive)"),
      parentClass: z.string().optional().describe("Only show classes that inherit from this class (e.g. 'Actor', 'ActorComponent')"),
      limit: z.number().optional().default(100).describe("Maximum number of results (default: 100, max: 500)"),
    },
    async ({ filter, parentClass, limit }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = {};
      if (filter) body.filter = filter;
      if (parentClass) body.parentClass = parentClass;
      if (limit !== undefined) body.limit = limit;

      const data = await uePost("/api/list-classes", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (data.truncated) {
        lines.push(`Showing ${data.count} of ${data.totalMatched} matching classes (limit: ${data.limit}).\n`);
      } else {
        lines.push(`Found ${data.count} classes.\n`);
      }

      for (const cls of data.classes) {
        const tags: string[] = [];
        if (cls.isBlueprint) tags.push("BP");
        if (cls.flags?.length) tags.push(...cls.flags);
        const tagStr = tags.length ? ` [${tags.join(", ")}]` : "";
        lines.push(`${cls.name}${tagStr} : ${cls.parentClass || "none"}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_functions",
    "List Blueprint-callable functions on a UE5 class, including parameter signatures and return types. Use this to discover function names for add_node(CallFunction, functionName=...).",
    {
      className: z.string().describe("Class name (e.g. 'KismetSystemLibrary', 'KismetMathLibrary', 'Actor')"),
      filter: z.string().optional().describe("Substring to match against function name (case-insensitive)"),
    },
    async ({ className, filter }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { className };
      if (filter) body.filter = filter;

      const data = await uePost("/api/list-functions", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`${data.className}: ${data.count} callable functions.\n`);

      for (const fn of data.functions) {
        const tags: string[] = [];
        if (fn.isPure) tags.push("pure");
        if (fn.isStatic) tags.push("static");
        if (fn.isEvent) tags.push("event");
        if (fn.isConst) tags.push("const");
        const tagStr = tags.length ? ` [${tags.join(", ")}]` : "";

        const params = fn.parameters
          .filter((p: any) => !p.isOutput)
          .map((p: any) => `${p.name}: ${p.type}`)
          .join(", ");
        const outParams = fn.parameters
          .filter((p: any) => p.isOutput)
          .map((p: any) => `${p.name}: ${p.type}`)
          .join(", ");

        let sig = `${fn.name}(${params})`;
        if (fn.returnType) sig += ` -> ${fn.returnType}`;
        if (outParams) sig += ` [out: ${outParams}]`;
        sig += tagStr;

        if (fn.definedIn && fn.definedIn !== data.className) {
          sig += ` (from ${fn.definedIn})`;
        }

        lines.push(sig);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_properties",
    "List properties on a UE5 class, including types and property flags (BlueprintVisible, EditAnywhere, etc.).",
    {
      className: z.string().describe("Class name (e.g. 'Actor', 'CharacterMovementComponent')"),
      filter: z.string().optional().describe("Substring to match against property name (case-insensitive)"),
    },
    async ({ className, filter }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { className };
      if (filter) body.filter = filter;

      const data = await uePost("/api/list-properties", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`${data.className}: ${data.count} properties.\n`);

      for (const prop of data.properties) {
        const flagStr = prop.flags?.length ? ` [${prop.flags.join(", ")}]` : "";
        let line = `${prop.name}: ${prop.type}${flagStr}`;
        if (prop.definedIn && prop.definedIn !== data.className) {
          line += ` (from ${prop.definedIn})`;
        }
        lines.push(line);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
