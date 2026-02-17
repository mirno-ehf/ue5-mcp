import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost, ueGet } from "../ue-bridge.js";

export function registerMaterialMutationTools(server: McpServer): void {
  // ---------------------------------------------------------------------------
  // Phase 2: Material Mutations
  // ---------------------------------------------------------------------------

  server.tool(
    "create_material",
    "Create a new Material asset.",
    {
      name: z.string().describe("Material asset name (e.g. 'M_MyMaterial')"),
      packagePath: z.string().default("/Game").describe("Package path to create the material in (e.g. '/Game/Materials')"),
      domain: z.enum(["Surface", "DeferredDecal", "LightFunction", "Volume", "PostProcess", "UI"]).optional().describe("Material domain"),
      blendMode: z.enum(["Opaque", "Masked", "Translucent", "Additive", "Modulate"]).optional().describe("Blend mode"),
      twoSided: z.boolean().optional().describe("Whether the material is two-sided"),
    },
    async ({ name, packagePath, domain, blendMode, twoSided }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { name, packagePath };
      if (domain) body.domain = domain;
      if (blendMode) body.blendMode = blendMode;
      if (twoSided !== undefined) body.twoSided = twoSided;

      const data = await uePost("/api/create-material", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Created material ${data.name || name} at ${data.packagePath || packagePath}`);
      if (data.domain) lines.push(`Domain: ${data.domain}`);
      if (data.blendMode) lines.push(`Blend mode: ${data.blendMode}`);
      if (data.twoSided !== undefined) lines.push(`Two-sided: ${data.twoSided}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use add_material_expression to add nodes to the material graph`);
      lines.push(`  2. Use connect_material_pins to wire expressions together`);
      lines.push(`  3. Use set_material_property to adjust material settings`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_material_property",
    "Set a top-level property on a Material (domain, blendMode, twoSided, shadingModel).",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      property: z.string().describe("Property name to set (e.g. 'domain', 'blendMode', 'twoSided', 'shadingModel')"),
      value: z.string().describe("New value for the property"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, property, value, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, property, value };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/set-material-property", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Material: ${data.material || material}`);
      lines.push(`Property: ${data.property || property}`);
      lines.push(`Old value: ${data.oldValue ?? "(empty)"} -> New value: ${data.newValue ?? value}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_graph to verify the changes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_material_expression",
    "Add a new expression node to a Material's graph.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      expressionClass: z.string().describe("Expression type: Constant, ScalarParameter, VectorParameter, TextureSample, TextureSampleParameter2D, TextureCoordinate, Add, Multiply, Lerp, Clamp, OneMinus, Power, Time, WorldPosition, AppendVector, ComponentMask, Custom, StaticSwitchParameter, MaterialFunctionCall, Constant3Vector, Constant4Vector"),
      posX: z.number().default(0).describe("X position in the graph editor"),
      posY: z.number().default(0).describe("Y position in the graph editor"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, expressionClass, posX, posY, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, expressionClass, posX, posY };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/add-material-expression", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Expression added: ${data.expressionClass || expressionClass}`);
      lines.push(`Material: ${data.material || material}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.posX !== undefined && data.posY !== undefined) lines.push(`Position: (${data.posX}, ${data.posY})`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (data.pins?.length) {
        lines.push(`\nPins:`);
        for (const pin of data.pins) {
          const dir = pin.direction === "Output" ? "\u2192" : "\u2190";
          lines.push(`  ${dir} ${pin.name}: ${pin.type}${pin.subtype ? ` (${pin.subtype})` : ""}`);
        }
      }

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use set_expression_value to configure the expression`);
        lines.push(`  2. Use connect_material_pins to wire it to other nodes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "delete_material_expression",
    "Delete an expression node from a Material's graph.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      nodeId: z.string().describe("GUID of the expression node to delete"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, nodeId, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, nodeId };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/delete-material-expression", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Expression deleted.`);
      lines.push(`Material: ${data.material || material}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.expressionClass) lines.push(`Expression class: ${data.expressionClass}`);
      if (data.disconnectedPins !== undefined) lines.push(`Disconnected pins: ${data.disconnectedPins}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_graph to verify the changes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "connect_material_pins",
    "Connect two pins in a Material's graph.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      sourceNodeId: z.string().describe("GUID of the source expression node"),
      sourcePinName: z.string().describe("Name of the output pin on the source node"),
      targetNodeId: z.string().describe("GUID of the target expression node (or 'Result' for the material result node)"),
      targetPinName: z.string().describe("Name of the input pin on the target node"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, sourceNodeId, sourcePinName, targetNodeId, targetPinName, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, sourceNodeId, sourcePinName, targetNodeId, targetPinName };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/connect-material-pins", body);
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.availablePins) {
          msg += `\nAvailable pins: ${data.availablePins.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Connection ${data.success ? "succeeded" : "failed"}.`);
      lines.push(`Material: ${data.material || material}`);
      lines.push(`${sourcePinName} \u2192 ${targetPinName}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_graph to verify the connection`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "disconnect_material_pin",
    "Disconnect all links from a specific pin in a Material's graph.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      nodeId: z.string().describe("GUID of the expression node containing the pin"),
      pinName: z.string().describe("Name of the pin to disconnect"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, nodeId, pinName, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, nodeId, pinName };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/disconnect-material-pin", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Disconnected ${data.disconnectedCount ?? 0} link(s).`);
      lines.push(`Material: ${data.material || material}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_graph to verify the changes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_expression_value",
    "Set the value of a material expression (constants, parameter defaults, custom code, etc.).",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      nodeId: z.string().describe("GUID of the expression node"),
      value: z.union([
        z.number(),
        z.object({ r: z.number(), g: z.number(), b: z.number(), a: z.number().optional() }),
        z.string(),
      ]).describe("Value to set: number (for scalar), {r,g,b,a?} (for vector/color), or string"),
      parameterName: z.string().optional().describe("Parameter name override (for parameter expressions)"),
      code: z.string().optional().describe("Custom HLSL code (for Custom expression nodes)"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, nodeId, value, parameterName, code, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, nodeId, value };
      if (parameterName) body.parameterName = parameterName;
      if (code) body.code = code;
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/set-expression-value", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Expression value set.`);
      lines.push(`Material: ${data.material || material}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.expressionClass) lines.push(`Expression class: ${data.expressionClass}`);
      if (data.oldValue !== undefined) lines.push(`Old value: ${typeof data.oldValue === "object" ? JSON.stringify(data.oldValue) : data.oldValue}`);
      if (data.newValue !== undefined) lines.push(`New value: ${typeof data.newValue === "object" ? JSON.stringify(data.newValue) : data.newValue}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_graph to verify the changes`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "move_material_expression",
    "Move a material expression node to a new position in the graph editor.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      nodeId: z.string().describe("GUID of the expression node to move"),
      posX: z.number().describe("New X position"),
      posY: z.number().describe("New Y position"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material"),
    },
    async ({ material, nodeId, posX, posY, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, nodeId, posX, posY };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/move-material-expression", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Expression repositioned.`);
      lines.push(`Material: ${data.material || material}`);
      if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
      if (data.oldPosX !== undefined && data.oldPosY !== undefined) {
        lines.push(`Position: (${data.oldPosX}, ${data.oldPosY}) -> (${data.newPosX ?? posX}, ${data.newPosY ?? posY})`);
      } else {
        lines.push(`Position: (${data.newPosX ?? posX}, ${data.newPosY ?? posY})`);
      }
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // Phase 3: Material Instances
  // ---------------------------------------------------------------------------

  server.tool(
    "create_material_instance",
    "Create a new Material Instance asset with a specified parent material.",
    {
      name: z.string().describe("Material Instance asset name (e.g. 'MI_MyMaterial')"),
      packagePath: z.string().default("/Game").describe("Package path to create the instance in (e.g. '/Game/Materials')"),
      parentMaterial: z.string().describe("Parent material name or package path (e.g. 'M_MyMaterial')"),
    },
    async ({ name, packagePath, parentMaterial }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { name, packagePath, parentMaterial };

      const data = await uePost("/api/create-material-instance", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Created material instance ${data.name || name} at ${data.packagePath || packagePath}`);
      if (data.parentMaterial) lines.push(`Parent: ${data.parentMaterial}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use set_material_instance_parameter to override parameter values`);
      lines.push(`  2. Use get_material_instance_parameters to inspect available parameters`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_material_instance_parameter",
    "Override a parameter value in a Material Instance.",
    {
      materialInstance: z.string().describe("Material Instance name or package path (e.g. 'MI_MyMaterial')"),
      parameterName: z.string().describe("Name of the parameter to override"),
      value: z.union([
        z.number(),
        z.object({ r: z.number(), g: z.number(), b: z.number(), a: z.number().optional() }),
        z.string(),
        z.boolean(),
      ]).describe("Value to set: number (scalar), {r,g,b,a?} (vector/color), string (texture path), or boolean (static switch)"),
      type: z.enum(["scalar", "vector", "texture", "staticSwitch"]).optional().describe("Parameter type hint (auto-detected if omitted)"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material Instance"),
    },
    async ({ materialInstance, parameterName, value, type, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { materialInstance, parameterName, value };
      if (type) body.type = type;
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/set-material-instance-parameter", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Parameter override set.`);
      lines.push(`Material Instance: ${data.materialInstance || materialInstance}`);
      lines.push(`Parameter: ${data.parameterName || parameterName}`);
      if (data.type) lines.push(`Type: ${data.type}`);
      if (data.oldValue !== undefined) lines.push(`Old value: ${typeof data.oldValue === "object" ? JSON.stringify(data.oldValue) : data.oldValue}`);
      if (data.newValue !== undefined) lines.push(`New value: ${typeof data.newValue === "object" ? JSON.stringify(data.newValue) : data.newValue}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_instance_parameters to verify all overrides`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_material_instance_parameters",
    "Get all parameters of a Material Instance, showing which are overridden vs inherited from parent.",
    {
      name: z.string().describe("Material Instance name or package path (e.g. 'MI_MyMaterial')"),
    },
    async ({ name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/material-instance-params", { name });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Material Instance: ${data.name || name}`);
      if (data.parentMaterial) lines.push(`Parent: ${data.parentMaterial}`);
      if (data.parentChain?.length) {
        lines.push(`Parent chain: ${data.parentChain.join(" -> ")}`);
      }

      const formatParamValue = (val: any): string => {
        if (val === undefined || val === null) return "(default)";
        if (typeof val === "object") return JSON.stringify(val);
        return String(val);
      };

      if (data.scalarParameters?.length) {
        lines.push(`\nScalar Parameters:`);
        for (const p of data.scalarParameters) {
          const override = p.overridden ? " [OVERRIDDEN]" : "";
          lines.push(`  ${p.name}: ${formatParamValue(p.value)}${override}`);
        }
      }

      if (data.vectorParameters?.length) {
        lines.push(`\nVector Parameters:`);
        for (const p of data.vectorParameters) {
          const override = p.overridden ? " [OVERRIDDEN]" : "";
          lines.push(`  ${p.name}: ${formatParamValue(p.value)}${override}`);
        }
      }

      if (data.textureParameters?.length) {
        lines.push(`\nTexture Parameters:`);
        for (const p of data.textureParameters) {
          const override = p.overridden ? " [OVERRIDDEN]" : "";
          lines.push(`  ${p.name}: ${formatParamValue(p.value)}${override}`);
        }
      }

      if (data.staticSwitchParameters?.length) {
        lines.push(`\nStatic Switch Parameters:`);
        for (const p of data.staticSwitchParameters) {
          const override = p.overridden ? " [OVERRIDDEN]" : "";
          lines.push(`  ${p.name}: ${formatParamValue(p.value)}${override}`);
        }
      }

      if (!data.scalarParameters?.length && !data.vectorParameters?.length &&
          !data.textureParameters?.length && !data.staticSwitchParameters?.length) {
        lines.push(`\nNo parameters found.`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "reparent_material_instance",
    "Change the parent of a Material Instance to a different Material or Material Instance.",
    {
      materialInstance: z.string().describe("Material Instance name or package path (e.g. 'MI_MyMaterial')"),
      newParent: z.string().describe("New parent material name or package path (e.g. 'M_NewParent')"),
      dryRun: z.boolean().optional().describe("If true, preview changes without modifying the Material Instance"),
    },
    async ({ materialInstance, newParent, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { materialInstance, newParent };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/reparent-material-instance", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]`);
      lines.push(`Material Instance reparented.`);
      lines.push(`Material Instance: ${data.materialInstance || materialInstance}`);
      if (data.oldParent) lines.push(`Old parent: ${data.oldParent}`);
      lines.push(`New parent: ${data.newParent || newParent}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (!dryRun) {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use get_material_instance_parameters to check parameter compatibility`);
        lines.push(`  2. Use set_material_instance_parameter to update overrides if needed`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // Phase 4: Create Material Function
  // ---------------------------------------------------------------------------

  server.tool(
    "create_material_function",
    "Create a new Material Function asset.",
    {
      name: z.string().describe("Material Function asset name (e.g. 'MF_MyFunction')"),
      packagePath: z.string().default("/Game").describe("Package path to create the function in (e.g. '/Game/Materials/Functions')"),
      description: z.string().optional().describe("Description of the material function"),
    },
    async ({ name, packagePath, description }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { name, packagePath };
      if (description) body.description = description;

      const data = await uePost("/api/create-material-function", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Created material function ${data.name || name} at ${data.packagePath || packagePath}`);
      if (data.description) lines.push(`Description: ${data.description}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use add_material_expression to add expression nodes to the function`);
      lines.push(`  2. Use connect_material_pins to wire expressions together`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // Phase 5: Snapshot/Diff/Restore
  // ---------------------------------------------------------------------------

  server.tool(
    "snapshot_material_graph",
    "Take a snapshot of a Material's graph for later comparison or restoration.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
    },
    async ({ material }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/snapshot-material-graph", { material });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Snapshot ${data.snapshotId} created for material ${data.material || material}`);
      if (data.expressionCount !== undefined) lines.push(`Expressions captured: ${data.expressionCount}`);
      if (data.connectionCount !== undefined) lines.push(`Connections captured: ${data.connectionCount}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Make your changes to the material`);
      lines.push(`  2. Use diff_material_graph to see what changed`);
      lines.push(`  3. Use restore_material_graph to reconnect severed pins`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "diff_material_graph",
    "Compare a Material's current graph against a previously taken snapshot.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      snapshotId: z.string().describe("Snapshot ID from snapshot_material_graph"),
    },
    async ({ material, snapshotId }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/diff-material-graph", { material, snapshotId });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`# Diff: ${data.material || material} vs ${data.snapshotId || snapshotId}`);

      if (data.severedConnections?.length) {
        lines.push(`\nSevered connections (${data.severedConnections.length}):`);
        for (const sc of data.severedConnections) {
          lines.push(`  ${sc.sourceNodeTitle || sc.sourceNodeId}.${sc.sourcePinName} was -> ${sc.targetNodeTitle || sc.targetNodeId}.${sc.targetPinName}`);
        }
      }

      if (data.newConnections?.length) {
        lines.push(`\nNew connections (${data.newConnections.length}):`);
        for (const nc of data.newConnections) {
          lines.push(`  ${nc.sourceNodeTitle || nc.sourceNodeId}.${nc.sourcePinName} -> ${nc.targetNodeTitle || nc.targetNodeId}.${nc.targetPinName}`);
        }
      }

      if (data.missingNodes?.length) {
        lines.push(`\nMissing nodes (${data.missingNodes.length}):`);
        for (const mn of data.missingNodes) {
          lines.push(`  ${mn.nodeId} (${mn.nodeTitle || mn.expressionClass || "unknown"})`);
        }
      }

      if (data.newNodes?.length) {
        lines.push(`\nNew nodes (${data.newNodes.length}):`);
        for (const nn of data.newNodes) {
          lines.push(`  ${nn.nodeId} (${nn.nodeTitle || nn.expressionClass || "unknown"})`);
        }
      }

      if (!data.severedConnections?.length && !data.newConnections?.length &&
          !data.missingNodes?.length && !data.newNodes?.length) {
        lines.push(`\nNo changes detected.`);
      }

      if (data.summary) {
        lines.push(`\nSummary: ${data.summary.severed ?? 0} severed, ${data.summary.new ?? 0} new connections, ${data.summary.missingNodes ?? 0} missing nodes`);
      }

      lines.push(`\nNext steps:`);
      if (data.severedConnections?.length) {
        lines.push(`  1. Use restore_material_graph to reconnect severed pins`);
      } else {
        lines.push(`  1. No action needed \u2014 graph is intact`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "restore_material_graph",
    "Restore severed connections in a Material's graph from a snapshot.",
    {
      material: z.string().describe("Material name or package path (e.g. 'M_MyMaterial')"),
      snapshotId: z.string().describe("Snapshot ID from snapshot_material_graph"),
      dryRun: z.boolean().optional().describe("If true, preview reconnections without making changes"),
    },
    async ({ material, snapshotId, dryRun }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { material, snapshotId };
      if (dryRun) body.dryRun = true;

      const data = await uePost("/api/restore-material-graph", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      if (dryRun) lines.push(`[DRY RUN - no changes saved]\n`);

      lines.push(`Restore: ${data.material || material} from ${data.snapshotId || snapshotId}`);
      lines.push(`Reconnected: ${data.reconnected ?? 0}/${(data.reconnected ?? 0) + (data.failed ?? 0)}`);
      if ((data.failed ?? 0) > 0) {
        lines.push(`Failed: ${data.failed}`);
      }

      if (data.details?.length) {
        lines.push(`\nDetails:`);
        for (const d of data.details) {
          const status = d.result === "ok" ? "OK" : "FAILED";
          const reason = d.reason ? ` (${d.reason})` : "";
          lines.push(`  ${status}: ${d.sourcePinName} -> ${d.targetNodeTitle || d.targetNodeId}.${d.targetPinName}${reason}`);
        }
      }

      if (!dryRun && data.saved !== undefined) {
        lines.push(`\nSaved: ${data.saved}`);
      }

      lines.push(`\nNext steps:`);
      if (dryRun) {
        lines.push(`  1. Re-run restore_material_graph without dryRun to apply changes`);
      }
      if ((data.failed ?? 0) > 0) {
        lines.push(`  1. Fix ${data.failed} failed reconnection(s) manually with connect_material_pins`);
      }
      lines.push(`  2. Use get_material_graph to verify the final state`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
