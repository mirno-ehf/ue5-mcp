import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";
import { TYPE_NAME_DOCS, formatUpdatedState } from "../helpers.js";

export function registerMutationTools(server: McpServer): void {
  server.tool(
    "replace_function_calls",
    "In a Blueprint, redirect all function call nodes from one function library class to another (matched by function name). Reports which pin connections were broken due to type changes. Use this for migrating Blueprints from one function library to another. Pass dryRun=true to preview changes without saving.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientJson')"),
      oldClass: z.string().describe("Current function library class name (e.g. 'FL_StateParsers')"),
      newClass: z.string().describe("New function library class name (e.g. 'StateParsersLibrary')"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Blueprint"),
    },
    async ({ blueprint, oldClass, newClass, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, oldClass, newClass };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/replace-function-calls", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Replaced: ${data.replacedCount} function call node(s)`);

      if (data.saved !== undefined) {
        lines.push(`Saved: ${data.saved}`);
      }

      if (data.message) {
        lines.push(data.message);
      }

      if (data.brokenConnectionCount > 0) {
        lines.push(`\nBroken connections (${data.brokenConnectionCount}):`);
        for (const bc of data.brokenConnections) {
          if (bc.type === "functionNotFound") {
            lines.push(`  WARNING: Function '${bc.functionName}' not found in new class (node ${bc.nodeId})`);
          } else if (bc.type === "connectionLost") {
            lines.push(`  BROKEN: ${bc.functionName} pin '${bc.pinName}' was connected to node ${bc.wasConnectedToNode}.${bc.wasConnectedToPin}`);
          }
        }
        lines.push("\nThese connections must be fixed manually in the editor.");
      }

      // Updated state (#11)
      lines.push(...formatUpdatedState(data));

      // Tool chaining hints (#12)
      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Verify with get_blueprint_graph to inspect the updated graphs`);
        lines.push(`  2. Run refresh_all_nodes to propagate pin type changes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "delete_asset",
    "Delete a .uasset file after confirming no remaining references. By default refuses to delete if the asset is still referenced. Use force=true to delete anyway (references become stale). Use find_asset_references first to check dependencies.",
    {
      assetPath: z.string().describe("Full asset path to delete (e.g. '/Game/Blueprints/WebUI/S_Vitals')"),
      force: z.boolean().optional().describe("If true, force-delete even if references exist. Stale references will remain and must be cleaned up manually."),
      batch: z.array(z.object({
        assetPath: z.string(),
        force: z.boolean().optional(),
      })).optional().describe("Batch mode: array of {assetPath, force?} objects. When provided, single params are ignored."),
    },
    async ({ assetPath, force, batch }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = batch
        ? { batch }
        : { assetPath };
      if (force && !batch) body.force = true;

      const data = await uePost("/api/delete-asset", body);

      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.referencers) {
          // Classify live vs stale references (#16)
          const liveRefs = data.liveReferencers || [];
          const staleRefs = data.staleReferencers || [];
          if (liveRefs.length > 0 || staleRefs.length > 0) {
            if (liveRefs.length > 0) {
              msg += `\n\nLive references (${liveRefs.length}) \u2014 these assets actively use this asset:`;
              msg += liveRefs.map((r: string) => `\n  ${r}`).join("");
            }
            if (staleRefs.length > 0) {
              msg += `\n\nStale references (${staleRefs.length}) \u2014 these may be outdated/cached:`;
              msg += staleRefs.map((r: string) => `\n  ${r}`).join("");
            }
            msg += `\n\nNext steps:`;
            msg += `\n  - Fix live references by updating the referencing Blueprints`;
            msg += `\n  - Use force=true to delete despite stale references`;
            msg += `\n  - Or run find_asset_references to inspect each one`;
          } else {
            msg += `\n\nStill referenced by (${data.referencerCount}):\n`;
            msg += data.referencers.map((r: string) => `  ${r}`).join("\n");
            msg += `\n\nNext steps:`;
            msg += `\n  - Update or remove references in the listed assets first`;
            msg += `\n  - Or use force=true to force-delete (references become stale)`;
          }
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      if (data.results) {
        // Batch response
        const lines: string[] = [`Batch delete: ${data.results.length} operation(s)`];
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED ${r.assetPath}: ${r.error}`);
          } else {
            lines.push(`  DELETED ${r.assetPath}`);
          }
        }
        // Tool chaining hints (#12)
        lines.push(`\nNext steps:`);
        lines.push(`  1. Verify no orphaned references remain with find_asset_references`);
        return { content: [{ type: "text" as const, text: lines.join("\n") }] };
      }

      const lines: string[] = [];
      lines.push(`Asset deleted successfully.`);
      lines.push(`Path: ${data.assetPath}`);
      lines.push(`File: ${data.filename}`);

      // Show warning-based reference info when force was used (#1)
      if (data.warnings?.length) {
        lines.push(`\nWarnings:`);
        for (const w of data.warnings) {
          lines.push(`  \u26A0 ${w}`);
        }
      }
      if (data.forcedReferencers?.length) {
        lines.push(`\nForce-deleted despite references from:`);
        for (const ref of data.forcedReferencers) {
          lines.push(`  ${ref}`);
        }
        lines.push(`These references are now stale and should be cleaned up.`);
      }

      // Tool chaining hints (#12)
      lines.push(`\nNext steps:`);
      lines.push(`  1. Verify no orphaned references remain with find_asset_references`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "connect_pins",
    "Wire two pins together in a Blueprint graph. Uses type-validated connection (TryCreateConnection) so incompatible types will fail with details. Get node IDs and pin names from get_blueprint_graph first.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientJson')"),
      sourceNodeId: z.string().describe("GUID of the source node (from get_blueprint_graph node 'id' field)"),
      sourcePinName: z.string().describe("Name of the output pin on the source node"),
      targetNodeId: z.string().describe("GUID of the target node"),
      targetPinName: z.string().describe("Name of the input pin on the target node"),
      batch: z.array(z.object({
        blueprint: z.string(),
        sourceNodeId: z.string(),
        sourcePinName: z.string(),
        targetNodeId: z.string(),
        targetPinName: z.string(),
      })).optional().describe("Batch mode: array of connection objects. When provided, single params are ignored."),
    },
    async ({ blueprint, sourceNodeId, sourcePinName, targetNodeId, targetPinName, batch }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = batch
        ? { batch }
        : { blueprint, sourceNodeId, sourcePinName, targetNodeId, targetPinName };

      const data = await uePost("/api/connect-pins", body);

      if (data.error && !data.success) {
        let msg = `Error: ${data.error}`;
        if (data.availablePins) {
          msg += `\nAvailable pins: ${data.availablePins.join(", ")}`;
        }
        if (data.sourcePinType) msg += `\nSource pin type: ${data.sourcePinType}${data.sourcePinSubtype ? ` (${data.sourcePinSubtype})` : ""}`;
        if (data.targetPinType) msg += `\nTarget pin type: ${data.targetPinType}${data.targetPinSubtype ? ` (${data.targetPinSubtype})` : ""}`;
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];

      if (data.results) {
        // Batch response
        lines.push(`Batch connect: ${data.results.length} operation(s)`);
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED: ${r.error}`);
          } else {
            lines.push(`  OK: ${r.sourcePinName} \u2192 ${r.targetPinName}`);
          }
        }
        if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      } else {
        lines.push(`Connection ${data.success ? "succeeded" : "failed"}.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        lines.push(`Source pin type: ${data.sourcePinType}${data.sourcePinSubtype ? ` (${data.sourcePinSubtype})` : ""}`);
        lines.push(`Target pin type: ${data.targetPinType}${data.targetPinSubtype ? ` (${data.targetPinSubtype})` : ""}`);
        if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      }

      // Updated state (#11)
      lines.push(...formatUpdatedState(data));

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "disconnect_pin",
    "Break connections on a specific pin. By default breaks ALL connections on the pin. Optionally specify targetNodeId + targetPinName to break only a single specific link.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().describe("GUID of the node containing the pin"),
      pinName: z.string().describe("Name of the pin to disconnect"),
      targetNodeId: z.string().optional().describe("GUID of a specific connected node to disconnect from (optional)"),
      targetPinName: z.string().optional().describe("Pin name on the target node to disconnect from (optional, required if targetNodeId is set)"),
    },
    async ({ blueprint, nodeId, pinName, targetNodeId, targetPinName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, nodeId, pinName };
      if (targetNodeId) body.targetNodeId = targetNodeId;
      if (targetPinName) body.targetPinName = targetPinName;

      const data = await uePost("/api/disconnect-pin", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Disconnected ${data.disconnectedCount} link(s).`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Saved: ${data.saved}`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "change_struct_node_type",
    `Change a BreakStruct or MakeStruct node to use a different struct type. Reconstructs the node and attempts to reconnect pins by matching property names. Get node IDs from get_blueprint_graph first. ${TYPE_NAME_DOCS}`,
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientJson')"),
      nodeId: z.string().describe("GUID of the BreakStruct or MakeStruct node"),
      newType: z.string().describe("New struct type name with F prefix (e.g. 'FVitals', 'FSkinState')"),
    },
    async ({ blueprint, nodeId, newType }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/change-struct-node-type", { blueprint, nodeId, newType });

      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Struct node type changed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Node: ${data.nodeId} (${data.nodeClass})`);
      lines.push(`New type: ${data.newStructType}`);
      lines.push(`Reconnected: ${data.reconnected}, Failed: ${data.failed}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (data.reconnectDetails?.length) {
        lines.push(`\nReconnection details:`);
        for (const d of data.reconnectDetails) {
          const status = d.connected ? "OK" : "FAILED";
          lines.push(`  ${d.property}: ${status}${d.reason ? ` (${d.reason})` : ""}`);
        }
      }

      // Updated state (#11)
      lines.push(...formatUpdatedState(data));

      // Tool chaining hints (#12)
      lines.push(`\nNext steps:`);
      lines.push(`  1. Run refresh_all_nodes to propagate type changes throughout the Blueprint`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "refresh_all_nodes",
    "Refresh all nodes in a Blueprint to update pin types and connections after modifications (e.g. after replace_function_calls or change_variable_type). Recompiles and saves the Blueprint.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientManager')"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/refresh-all-nodes", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Refresh ${data.success ? "succeeded" : "completed with issues"}.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Graphs: ${data.graphCount}, Nodes: ${data.nodeCount}`);
      lines.push(`Saved: ${data.saved}`);
      if (data.warning) lines.push(`Warning: ${data.warning}`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "delete_node",
    "Remove a node from a Blueprint graph. Disconnects all pins and removes the node. Use get_blueprint_graph to find node IDs first. Entry/root nodes (FunctionEntry, Event, CustomEvent) cannot be deleted as this would leave the graph uncompilable.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().describe("GUID of the node to delete (from get_blueprint_graph node 'id' field)"),
    },
    async ({ blueprint, nodeId }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/delete-node", { blueprint, nodeId });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Node removed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.nodeClass) lines.push(`Node class: ${data.nodeClass}`);
      if (data.nodeTitle) lines.push(`Node title: ${data.nodeTitle}`);
      if (data.disconnectedPins !== undefined) lines.push(`Disconnected pins: ${data.disconnectedPins}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_node",
    "Add a new node to a Blueprint graph. Supports: BreakStruct, MakeStruct, CallFunction, VariableGet, VariableSet, DynamicCast, OverrideEvent, CallParentFunction, Branch, Sequence, CustomEvent, ForEachLoop, ForLoop, ForLoopWithBreak, WhileLoop, SpawnActorFromClass, Select, Comment, Reroute. For Delay/IsValid/PrintString, use CallFunction with className 'KismetSystemLibrary'.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      graph: z.string().describe("Graph name (e.g. 'EventGraph')"),
      nodeType: z.enum([
        "BreakStruct", "MakeStruct", "CallFunction", "VariableGet", "VariableSet",
        "DynamicCast", "OverrideEvent", "CallParentFunction",
        "Branch", "Sequence", "CustomEvent",
        "ForEachLoop", "ForLoop", "ForLoopWithBreak", "WhileLoop",
        "SpawnActorFromClass", "Select", "Comment", "Reroute"
      ]).describe("Type of node to add"),
      typeName: z.string().optional().describe("Struct type name for BreakStruct/MakeStruct (e.g. 'FVitals')"),
      functionName: z.string().optional().describe("Function name for CallFunction, OverrideEvent, or CallParentFunction (e.g. 'PrintString')"),
      className: z.string().optional().describe("Class name for CallFunction (e.g. 'KismetSystemLibrary')"),
      variableName: z.string().optional().describe("Variable name for VariableGet/VariableSet"),
      castTarget: z.string().optional().describe("Target class name for DynamicCast (e.g. 'BP_PatientJson')"),
      eventName: z.string().optional().describe("Event name for CustomEvent (e.g. 'OnDataReady')"),
      actorClass: z.string().optional().describe("Actor class for SpawnActorFromClass (e.g. 'BP_Patient_Base'). Optional — can also be set via the class pin."),
      comment: z.string().optional().describe("Comment text for Comment node type"),
      width: z.number().optional().describe("Width for Comment node (default: 400)"),
      height: z.number().optional().describe("Height for Comment node (default: 200)"),
      posX: z.number().optional().describe("X position in the graph (optional)"),
      posY: z.number().optional().describe("Y position in the graph (optional)"),
    },
    async ({ blueprint, graph, nodeType, typeName, functionName, className, variableName, castTarget, eventName, actorClass, comment, width, height, posX, posY }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, nodeType };
      if (typeName) body.typeName = typeName;
      if (functionName) body.functionName = functionName;
      if (className) body.className = className;
      if (variableName) body.variableName = variableName;
      if (castTarget) body.castTarget = castTarget;
      if (eventName) body.eventName = eventName;
      if (actorClass) body.actorClass = actorClass;
      if (comment) body.comment = comment;
      if (width !== undefined) body.width = width;
      if (height !== undefined) body.height = height;
      if (posX !== undefined) body.posX = posX;
      if (posY !== undefined) body.posY = posY;

      const data = await uePost("/api/add-node", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (data.alreadyExists) {
        lines.push(`Node already exists (returning existing).`);
      } else {
        lines.push(`Node added successfully.`);
      }
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Graph: ${data.graph}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.nodeClass) lines.push(`Node class: ${data.nodeClass}`);
      if (data.nodeTitle) lines.push(`Node title: ${data.nodeTitle}`);

      if (data.node?.pins?.length) {
        lines.push(`\nPins:`);
        for (const pin of data.node.pins) {
          const dir = pin.direction === "Output" ? "\u2192" : "\u2190";
          lines.push(`  ${dir} ${pin.name}: ${pin.type}${pin.subtype ? ` (${pin.subtype})` : ""}`);
        }
      } else if (data.pins?.length) {
        lines.push(`\nPins:`);
        for (const pin of data.pins) {
          const dir = pin.direction === "Output" ? "\u2192" : "\u2190";
          lines.push(`  ${dir} ${pin.name}: ${pin.type}${pin.subtype ? ` (${pin.subtype})` : ""}`);
        }
      }

      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "rename_asset",
    "Rename or move an asset (Blueprint, Material, Material Instance, or Material Function) and update all references.",
    {
      assetPath: z.string().describe("Current full asset path (e.g. '/Game/Blueprints/Old/BP_MyActor' or '/Game/Materials/MI_Skin')"),
      newPath: z.string().describe("New full asset path (e.g. '/Game/Blueprints/New/BP_MyRenamedActor')"),
    },
    async ({ assetPath, newPath }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/rename-asset", { assetPath, newPath });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Asset renamed/moved successfully.`);
      lines.push(`From: ${data.oldPath || assetPath}`);
      lines.push(`To: ${data.newPath || newPath}`);
      if (data.referencesUpdated !== undefined) lines.push(`References updated: ${data.referencesUpdated}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_pin_default",
    "Set the default value of an input pin on a Blueprint node. Supports batch mode for setting multiple pins at once. Use this to set literal/constant values on pins that are not connected to other nodes.",
    {
      blueprint: z.string().optional().describe("Blueprint name or package path (required for single mode)"),
      nodeId: z.string().optional().describe("Node GUID (required for single mode)"),
      pinName: z.string().optional().describe("Pin name (required for single mode)"),
      value: z.string().optional().describe("Default value to set (required for single mode)"),
      batch: z.array(z.object({
        blueprint: z.string(),
        nodeId: z.string(),
        pinName: z.string(),
        value: z.string(),
      })).optional().describe("Batch mode: array of {blueprint, nodeId, pinName, value} objects. When provided, single params are ignored."),
    },
    async ({ blueprint, nodeId, pinName, value, batch }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = batch
        ? { batch }
        : { blueprint, nodeId, pinName, value };

      const data = await uePost("/api/set-pin-default", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];

      if (data.results) {
        // Batch response
        lines.push(`Batch set_pin_default: ${data.successCount}/${data.totalCount} succeeded.`);
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED ${r.nodeId || "?"}.${r.pinName || "?"}: ${r.error}`);
          } else {
            lines.push(`  OK ${r.nodeId}.${r.pinName}: ${r.oldValue || "(empty)"} -> ${r.newValue}`);
          }
        }
        if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      } else {
        lines.push(`Pin default set successfully.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        lines.push(`Node: ${data.nodeId}`);
        lines.push(`Pin: ${data.pinName}`);
        lines.push(`Old value: ${data.oldValue || "(empty)"}`);
        lines.push(`New value: ${data.newValue}`);
        if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "move_node",
    "Reposition one or more nodes in a Blueprint graph by setting their X/Y coordinates. Use batch mode with 'nodes' array for multiple moves in one call.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().optional().describe("Node GUID (for single-node mode)"),
      x: z.number().optional().describe("New X position (for single-node mode)"),
      y: z.number().optional().describe("New Y position (for single-node mode)"),
      nodes: z.array(z.object({
        nodeId: z.string(),
        x: z.number(),
        y: z.number(),
      })).optional().describe("Batch mode: array of {nodeId, x, y} objects"),
    },
    async ({ blueprint, nodeId, x, y, nodes }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint };
      if (nodes) {
        body.nodes = nodes;
      } else {
        if (nodeId) body.nodeId = nodeId;
        if (x !== undefined) body.x = x;
        if (y !== undefined) body.y = y;
      }

      const data = await uePost("/api/move-node", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];

      if (data.results) {
        // Batch response
        lines.push(`Batch move: ${data.movedCount}/${data.totalRequested} node(s) repositioned.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        for (const r of data.results) {
          if (r.error) {
            lines.push(`  FAILED ${r.nodeId}: ${r.error}`);
          } else {
            lines.push(`  OK ${r.nodeId}: (${r.oldX},${r.oldY}) -> (${r.newX},${r.newY})`);
          }
        }
      } else {
        lines.push(`Node repositioned successfully.`);
        lines.push(`Blueprint: ${data.blueprint}`);
        lines.push(`Node: ${data.nodeId}`);
        lines.push(`Position: (${data.oldX},${data.oldY}) -> (${data.newX},${data.newY})`);
      }
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_blueprint_default",
    "Set a default property value on a Blueprint's Class Default Object (CDO). Supports TSubclassOf (class references), object references, and simple types (bool, int, float, string, enum). For class/object values, provide the Blueprint asset name (e.g. 'MyWidget') or C++ class name.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'HUD_WebUIInterface')"),
      property: z.string().describe("Property name as declared in C++ or Blueprint (e.g. 'WebUIWidgetClass')"),
      value: z.string().describe("Value to set. For class properties: Blueprint name or C++ class name. For simple types: the literal value (e.g. 'true', '42', '0.5')"),
    },
    async ({ blueprint, property, value }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-blueprint-default", { blueprint, property, value });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Default property set successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Property: ${data.property} (${data.propertyType})`);
      lines.push(`Old value: ${data.oldValue || "(empty)"}`);
      lines.push(`New value: ${data.newValue}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "duplicate_nodes",
    "Duplicate one or more nodes within a Blueprint graph. Creates copies at an offset from the originals. The duplicated nodes are not connected to anything — use connect_pins to wire them up.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      graph: z.string().describe("Graph name (e.g. 'EventGraph')"),
      nodeIds: z.array(z.string()).describe("Array of node GUIDs to duplicate"),
      offsetX: z.number().optional().describe("X offset for duplicated nodes (default: 50)"),
      offsetY: z.number().optional().describe("Y offset for duplicated nodes (default: 50)"),
    },
    async ({ blueprint, graph, nodeIds, offsetX, offsetY }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, nodeIds };
      if (offsetX !== undefined) body.offsetX = offsetX;
      if (offsetY !== undefined) body.offsetY = offsetY;

      const data = await uePost("/api/duplicate-nodes", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Duplicated ${data.duplicatedCount} node(s).`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Graph: ${data.graph}`);

      if (data.nodes?.length) {
        lines.push(``);
        for (const n of data.nodes) {
          if (n.error) {
            lines.push(`  FAILED ${n.sourceNodeId}: ${n.error}`);
          } else {
            lines.push(`  ${n.sourceNodeId} -> ${n.newNodeId} at (${n.posX},${n.posY})`);
          }
        }
      }

      if (data.notFound?.length) {
        lines.push(`\nNot found: ${data.notFound.join(", ")}`);
      }

      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  connect_pins — wire the duplicated nodes to other nodes`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_node_comment",
    "Read the comment text (comment bubble) on a Blueprint node.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().describe("Node GUID"),
    },
    async ({ blueprint, nodeId }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/get-node-comment", { blueprint, nodeId });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Node: ${data.nodeId}`);
      lines.push(`Comment: ${data.comment || "(empty)"}`);
      lines.push(`Comment bubble visible: ${data.commentBubbleVisible}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_node_comment",
    "Set or clear the comment text (comment bubble) on a Blueprint node. When setting a non-empty comment, the comment bubble is automatically made visible and pinned.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      nodeId: z.string().describe("Node GUID"),
      comment: z.string().describe("Comment text to set (empty string to clear)"),
    },
    async ({ blueprint, nodeId, comment }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-node-comment", { blueprint, nodeId, comment });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Node comment set successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Node: ${data.nodeId}`);
      lines.push(`Old comment: ${data.oldComment || "(empty)"}`);
      lines.push(`New comment: ${data.newComment || "(empty)"}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
