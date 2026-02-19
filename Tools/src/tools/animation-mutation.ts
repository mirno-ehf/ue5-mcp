import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerAnimationTools(server: McpServer): void {
  // ---------------------------------------------------------------------------
  // create_anim_blueprint
  // ---------------------------------------------------------------------------

  server.tool(
    "create_anim_blueprint",
    "Create a new Animation Blueprint asset with a target skeleton.",
    {
      name: z.string().describe("Animation Blueprint name (e.g. 'ABP_MyCharacter')"),
      packagePath: z.string().default("/Game").describe("Package path (e.g. '/Game/Animations')"),
      skeleton: z.string().describe("Skeleton asset name or path. Use '__create_test_skeleton__' for testing."),
      parentClass: z.string().optional().describe("Parent class (default: AnimInstance)"),
    },
    async ({ name, packagePath, skeleton, parentClass }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { name, packagePath, skeleton };
      if (parentClass) body.parentClass = parentClass;

      const data = await uePost("/api/create-anim-blueprint", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Created Animation Blueprint: ${data.blueprintName || name}`);
      lines.push(`Path: ${data.assetPath || packagePath}`);
      lines.push(`Skeleton: ${data.targetSkeleton || skeleton}`);
      lines.push(`Parent: ${data.parentClass || "AnimInstance"}`);
      if (data.graphs?.length) lines.push(`Graphs: ${data.graphs.join(", ")}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use add_state_machine to add state machines to the AnimGraph`);
      lines.push(`  2. Use add_anim_state to add states to a state machine`);
      lines.push(`  3. Use add_anim_transition to connect states with transitions`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // add_anim_state
  // ---------------------------------------------------------------------------

  server.tool(
    "add_anim_state",
    "Add a state to a state machine graph in an Animation Blueprint.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      stateName: z.string().describe("Name for the new state"),
      animationAsset: z.string().optional().describe("Animation sequence asset to assign to the state"),
      posX: z.number().optional().describe("X position in graph"),
      posY: z.number().optional().describe("Y position in graph"),
    },
    async ({ blueprint, graph, stateName, animationAsset, posX, posY }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, stateName };
      if (animationAsset) body.animationAsset = animationAsset;
      if (posX !== undefined) body.posX = posX;
      if (posY !== undefined) body.posY = posY;

      const data = await uePost("/api/add-anim-state", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Added state "${data.stateName || stateName}" to ${data.graph || graph}`);
      lines.push(`Node ID: ${data.nodeId}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use add_anim_transition to connect this state to other states`);
      lines.push(`  2. Use set_state_animation to assign an animation to this state`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // remove_anim_state
  // ---------------------------------------------------------------------------

  server.tool(
    "remove_anim_state",
    "Remove a state and its connected transitions from a state machine graph.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      stateName: z.string().describe("Name of the state to remove"),
    },
    async ({ blueprint, graph, stateName }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-anim-state", { blueprint, graph, stateName });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Removed state "${data.removedState || stateName}"`);
      lines.push(`Removed transitions: ${data.removedTransitions ?? 0}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // add_anim_transition
  // ---------------------------------------------------------------------------

  server.tool(
    "add_anim_transition",
    "Add a transition between two states in a state machine graph.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      fromState: z.string().describe("Source state name"),
      toState: z.string().describe("Target state name"),
      crossfadeDuration: z.number().optional().describe("Crossfade duration in seconds (default: 0.2)"),
      priority: z.number().optional().describe("Transition priority order"),
      bBidirectional: z.boolean().optional().describe("Whether the transition is bidirectional"),
    },
    async ({ blueprint, graph, fromState, toState, crossfadeDuration, priority, bBidirectional }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, fromState, toState };
      if (crossfadeDuration !== undefined) body.crossfadeDuration = crossfadeDuration;
      if (priority !== undefined) body.priority = priority;
      if (bBidirectional !== undefined) body.bBidirectional = bBidirectional;

      const data = await uePost("/api/add-anim-transition", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Added transition: ${data.fromState || fromState} → ${data.toState || toState}`);
      lines.push(`Node ID: ${data.nodeId}`);
      lines.push(`Crossfade: ${data.crossfadeDuration ?? 0.2}s`);
      lines.push(`Priority: ${data.priorityOrder ?? 1}`);
      if (data.bBidirectional) lines.push(`Bidirectional: true`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use set_transition_rule to configure crossfade and priority`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // set_transition_rule
  // ---------------------------------------------------------------------------

  server.tool(
    "set_transition_rule",
    "Update properties on an existing transition between two states.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      fromState: z.string().describe("Source state name"),
      toState: z.string().describe("Target state name"),
      crossfadeDuration: z.number().optional().describe("Crossfade duration in seconds"),
      blendMode: z.number().optional().describe("Alpha blend option (0=Linear, 1=Cubic, 2=HermiteCubic, 3=Sinusoidal, 4=QuadraticInOut, 5=CubicInOut, 6=QuarticInOut, 7=QuinticInOut, 8=CircularIn, 9=CircularOut, 10=CircularInOut, 11=ExpIn, 12=ExpOut, 13=ExpInOut, 14=Custom)"),
      priorityOrder: z.number().optional().describe("Transition priority order"),
      logicType: z.number().optional().describe("Transition logic type (0=Standard, 1=Custom)"),
      bBidirectional: z.boolean().optional().describe("Whether the transition is bidirectional"),
    },
    async ({ blueprint, graph, fromState, toState, crossfadeDuration, blendMode, priorityOrder, logicType, bBidirectional }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, fromState, toState };
      if (crossfadeDuration !== undefined) body.crossfadeDuration = crossfadeDuration;
      if (blendMode !== undefined) body.blendMode = blendMode;
      if (priorityOrder !== undefined) body.priorityOrder = priorityOrder;
      if (logicType !== undefined) body.logicType = logicType;
      if (bBidirectional !== undefined) body.bBidirectional = bBidirectional;

      const data = await uePost("/api/set-transition-rule", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Updated transition: ${data.fromState || fromState} → ${data.toState || toState}`);
      lines.push(`Properties changed: ${data.propertiesChanged ?? 0}`);
      lines.push(`Crossfade: ${data.crossfadeDuration}s (blendMode: ${data.blendMode})`);
      lines.push(`Priority: ${data.priorityOrder}`);
      lines.push(`Bidirectional: ${data.bBidirectional}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // add_anim_node
  // ---------------------------------------------------------------------------

  server.tool(
    "add_anim_node",
    "Add an animation node (sequence player, blend space, state machine) to an AnimGraph.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().optional().describe("Target graph name (default: AnimGraph)"),
      nodeType: z.enum(["SequencePlayer", "BlendSpacePlayer", "StateMachine"]).describe("Type of anim node to add"),
      animationAsset: z.string().optional().describe("Animation/blend space asset name to assign"),
      posX: z.number().optional().describe("X position in graph"),
      posY: z.number().optional().describe("Y position in graph"),
    },
    async ({ blueprint, graph, nodeType, animationAsset, posX, posY }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, nodeType };
      if (graph) body.graph = graph;
      if (animationAsset) body.animationAsset = animationAsset;
      if (posX !== undefined) body.posX = posX;
      if (posY !== undefined) body.posY = posY;

      const data = await uePost("/api/add-anim-node", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Added ${data.nodeType || nodeType} node to ${data.graph || graph || "AnimGraph"}`);
      lines.push(`Node ID: ${data.nodeId}`);
      if (data.stateMachineGraph) lines.push(`State machine sub-graph: ${data.stateMachineGraph}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      if (nodeType === "StateMachine") {
        lines.push(`\nNext steps:`);
        lines.push(`  1. Use add_anim_state to add states to ${data.stateMachineGraph}`);
        lines.push(`  2. Use add_anim_transition to connect states`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // add_state_machine
  // ---------------------------------------------------------------------------

  server.tool(
    "add_state_machine",
    "Add a new state machine to the root AnimGraph of an Animation Blueprint.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      name: z.string().optional().describe("State machine name (default: NewStateMachine)"),
      posX: z.number().optional().describe("X position in graph"),
      posY: z.number().optional().describe("Y position in graph"),
    },
    async ({ blueprint, name, posX, posY }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint };
      if (name) body.name = name;
      if (posX !== undefined) body.posX = posX;
      if (posY !== undefined) body.posY = posY;

      const data = await uePost("/api/add-state-machine", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Added state machine to AnimGraph`);
      lines.push(`Node ID: ${data.nodeId}`);
      if (data.stateMachineGraph) lines.push(`Sub-graph: ${data.stateMachineGraph}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use add_anim_state to add states to ${data.stateMachineGraph || "the state machine"}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // set_state_animation
  // ---------------------------------------------------------------------------

  server.tool(
    "set_state_animation",
    "Set or replace the animation sequence played by a state in a state machine.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      stateName: z.string().describe("State name"),
      animationAsset: z.string().describe("Animation sequence asset name or path"),
    },
    async ({ blueprint, graph, stateName, animationAsset }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-state-animation", { blueprint, graph, stateName, animationAsset });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Set animation for state "${data.stateName || stateName}"`);
      lines.push(`Animation: ${data.animationAsset || animationAsset}`);
      lines.push(`Created new node: ${data.createdNewNode ?? false}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // create_blend_space
  // ---------------------------------------------------------------------------

  server.tool(
    "create_blend_space",
    "Create a new 2D Blend Space asset with a target skeleton.",
    {
      name: z.string().describe("Blend Space name (e.g. 'BS_Locomotion')"),
      packagePath: z.string().default("/Game").describe("Package path (e.g. '/Game/Animations')"),
      skeleton: z.string().describe("Skeleton asset name or path. Use '__create_test_skeleton__' for testing."),
    },
    async ({ name, packagePath, skeleton }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/create-blend-space", { name, packagePath, skeleton });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Created Blend Space: ${data.assetPath || `${packagePath}/${name}`}`);
      lines.push(`Skeleton: ${data.skeleton || skeleton}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Use set_blend_space_samples to add animation samples at X/Y coordinates`);
      lines.push(`  2. Use set_state_blend_space to wire it into an anim state`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // set_blend_space_samples
  // ---------------------------------------------------------------------------

  server.tool(
    "set_blend_space_samples",
    "Add animation samples to a 2D Blend Space at specific X/Y coordinates. Replaces all existing samples.",
    {
      blendSpace: z.string().describe("Blend Space asset name or path"),
      axisXName: z.string().optional().describe("Display name for the X axis"),
      axisXMin: z.number().optional().describe("Minimum value for X axis"),
      axisXMax: z.number().optional().describe("Maximum value for X axis"),
      axisYName: z.string().optional().describe("Display name for the Y axis"),
      axisYMin: z.number().optional().describe("Minimum value for Y axis"),
      axisYMax: z.number().optional().describe("Maximum value for Y axis"),
      samples: z.array(z.object({
        animationAsset: z.string().describe("Animation sequence asset name or path"),
        x: z.number().describe("X coordinate in blend space"),
        y: z.number().describe("Y coordinate in blend space"),
      })).describe("Array of animation samples with X/Y positions"),
    },
    async ({ blendSpace, axisXName, axisXMin, axisXMax, axisYName, axisYMin, axisYMax, samples }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blendSpace, samples };
      if (axisXName !== undefined) body.axisXName = axisXName;
      if (axisXMin !== undefined) body.axisXMin = axisXMin;
      if (axisXMax !== undefined) body.axisXMax = axisXMax;
      if (axisYName !== undefined) body.axisYName = axisYName;
      if (axisYMin !== undefined) body.axisYMin = axisYMin;
      if (axisYMax !== undefined) body.axisYMax = axisYMax;

      const data = await uePost("/api/set-blend-space-samples", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Set ${data.samplesSet ?? samples.length} samples on ${data.blendSpace || blendSpace}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // set_state_blend_space
  // ---------------------------------------------------------------------------

  server.tool(
    "set_state_blend_space",
    "Place a BlendSpacePlayer node inside an anim state, connect it to the Output Animation Pose, and optionally wire X/Y input pins to named variables.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
      graph: z.string().describe("State machine graph name"),
      stateName: z.string().describe("State name"),
      blendSpace: z.string().describe("Blend Space asset name or path"),
      xVariable: z.string().optional().describe("Blueprint float variable name to wire to X input"),
      yVariable: z.string().optional().describe("Blueprint float variable name to wire to Y input"),
    },
    async ({ blueprint, graph, stateName, blendSpace, xVariable, yVariable }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, graph, stateName, blendSpace };
      if (xVariable) body.xVariable = xVariable;
      if (yVariable) body.yVariable = yVariable;

      const data = await uePost("/api/set-state-blend-space", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Set blend space for state "${data.stateName || stateName}"`);
      lines.push(`Blend Space: ${data.blendSpace || blendSpace}`);
      lines.push(`Node ID: ${data.nodeId}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // list_anim_slots
  // ---------------------------------------------------------------------------

  server.tool(
    "list_anim_slots",
    "List all montage slot names used in an Animation Blueprint.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-anim-slots", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Animation slots in ${data.blueprint || blueprint}: ${data.count ?? 0}`);
      if (data.slots?.length) {
        for (const slot of data.slots) {
          lines.push(`  ${slot}`);
        }
      } else {
        lines.push(`  (no slots found)`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ---------------------------------------------------------------------------
  // list_sync_groups
  // ---------------------------------------------------------------------------

  server.tool(
    "list_sync_groups",
    "List all sync group names used in an Animation Blueprint.",
    {
      blueprint: z.string().describe("Animation Blueprint name or path"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-sync-groups", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Sync groups in ${data.blueprint || blueprint}: ${data.count ?? 0}`);
      if (data.syncGroups?.length) {
        for (const group of data.syncGroups) {
          lines.push(`  ${group}`);
        }
      } else {
        lines.push(`  (no sync groups found)`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
