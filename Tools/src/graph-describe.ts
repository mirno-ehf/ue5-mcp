import { flagType, formatVarType, formatParams } from "./helpers.js";

export interface NodeMap {
  [id: string]: any;
}

export function describeNode(node: any): string {
  const cls = node.class || "";
  if (node.nodeType === "CallParentFunction") {
    return `CALL PARENT ${node.functionName || node.title}`;
  }
  if (node.nodeType === "OverrideEvent") {
    return `OVERRIDE ${node.eventName || node.title}`;
  }
  if (cls.includes("CallFunction")) {
    const target = node.targetClass ? `${node.targetClass}.` : "";
    return `CALL ${target}${node.functionName || node.title}`;
  }
  if (node.nodeType === "VariableSet") return `SET ${node.variableName || node.title}`;
  if (node.nodeType === "VariableGet") return `GET ${node.variableName || node.title}`;
  if (node.nodeType === "Branch") return "IF";
  if (node.nodeType === "DynamicCast") return `CAST to ${node.castTarget || "?"}`;
  if (node.nodeType === "MacroInstance") return `MACRO ${node.macroName || node.title}`;
  if (cls.includes("AssignmentStatement")) return "ASSIGN";
  if (cls.includes("K2Node_Select")) return "SELECT";
  if (cls.includes("SwitchEnum") || cls.includes("SwitchInteger") || cls.includes("SwitchString") || cls.includes("Switch")) return `SWITCH`;
  if (cls.includes("ForEachLoop") || cls.includes("ForLoop")) return `FOR LOOP`;
  if (cls.includes("Sequence")) return "SEQUENCE";
  if (cls.includes("SpawnActor")) return "SPAWN ACTOR";
  if (cls.includes("CreateWidget")) return "CREATE WIDGET";
  if (cls.includes("Knot")) return null as any; // skip reroute nodes
  return node.title || cls;
}

/** Annotate a node description with data pin input connections (#10) */
export function annotateDataFlow(node: any, nodeMap: NodeMap): string {
  const dataInputs = (node.pins || []).filter(
    (p: any) => p.type !== "exec" && p.direction === "Input" && p.connections?.length > 0
  );
  if (dataInputs.length === 0) return "";

  const parts: string[] = [];
  for (const pin of dataInputs) {
    for (const conn of pin.connections) {
      const sourceNode = nodeMap[conn.nodeId];
      if (!sourceNode) continue;
      const sourceName = sourceNode.variableName || sourceNode.functionName || sourceNode.title || sourceNode.class || "?";
      const sourcePin = conn.pinName || "?";
      parts.push(`${pin.name}=${sourceName}.${sourcePin}`);
    }
  }
  if (parts.length === 0) return "";
  return `(${parts.join(", ")})`;
}

/** Annotate output data pins that feed into other nodes (#10) */
export function annotateDataOutputs(node: any, nodeMap: NodeMap): string[] {
  const lines: string[] = [];
  const dataOutputs = (node.pins || []).filter(
    (p: any) => p.type !== "exec" && p.direction === "Output" && p.connections?.length > 0
  );
  for (const pin of dataOutputs) {
    for (const conn of pin.connections) {
      const targetNode = nodeMap[conn.nodeId];
      if (!targetNode) continue;
      const targetName = targetNode.variableName || targetNode.functionName || targetNode.title || targetNode.class || "?";
      lines.push(`\u2192 ${pin.name} \u2192 [${targetName}.${conn.pinName || "?"}]`);
    }
  }
  return lines;
}

export function walkExecChain(startNodeId: string, nodeMap: NodeMap, visited: Set<string>, depth: number = 0): string[] {
  if (depth > 50 || visited.has(startNodeId)) return [];
  visited.add(startNodeId);

  const node = nodeMap[startNodeId];
  if (!node) return [];

  const lines: string[] = [];
  const indent = "  ".repeat(depth + 1);
  const desc = describeNode(node);
  const dataFlow = annotateDataFlow(node, nodeMap);

  // Find exec output pins (pins with type "exec" and direction "Output")
  const execOutPins = (node.pins || []).filter(
    (p: any) => p.type === "exec" && p.direction === "Output"
  );

  if (node.nodeType === "Branch") {
    // Special handling for branch: show IF with True/False paths
    lines.push(`${indent}IF:${dataFlow ? ` ${dataFlow}` : ""}`);
    for (const pin of execOutPins) {
      const label = pin.name || "?";
      if (pin.connections?.length) {
        lines.push(`${indent}  [${label}]:`);
        for (const conn of pin.connections) {
          lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, depth + 2));
        }
      }
    }
  } else if (node.class?.includes("Sequence")) {
    lines.push(`${indent}SEQUENCE:`);
    for (let i = 0; i < execOutPins.length; i++) {
      const pin = execOutPins[i];
      if (pin.connections?.length) {
        lines.push(`${indent}  [${i}]:`);
        for (const conn of pin.connections) {
          lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, depth + 2));
        }
      }
    }
  } else if (node.class?.includes("ForEachLoop") || node.class?.includes("ForLoop")) {
    if (desc) lines.push(`${indent}${desc}:${dataFlow ? ` ${dataFlow}` : ""}`);
    for (const pin of execOutPins) {
      const label = pin.name || "?";
      if (pin.connections?.length) {
        lines.push(`${indent}  [${label}]:`);
        for (const conn of pin.connections) {
          lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, depth + 2));
        }
      }
    }
  } else if (node.class?.includes("Switch")) {
    if (desc) lines.push(`${indent}${desc}:${dataFlow ? ` ${dataFlow}` : ""}`);
    for (const pin of execOutPins) {
      if (pin.connections?.length) {
        lines.push(`${indent}  [${pin.name}]:`);
        for (const conn of pin.connections) {
          lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, depth + 2));
        }
      }
    }
  } else {
    // Normal linear node: describe it and follow the first "then" exec pin
    if (desc) {
      lines.push(`${indent}${desc}${dataFlow ? ` ${dataFlow}` : ""}`);
      // Show data output connections (#10)
      const dataOuts = annotateDataOutputs(node, nodeMap);
      for (const dout of dataOuts) {
        lines.push(`${indent}  ${dout}`);
      }
    }
    // Follow exec chain: look for "then" pin or first exec output with connections
    const thenPin = execOutPins.find((p: any) => p.name === "then" || p.name === "execute" || p.name === "output") || execOutPins[0];
    if (thenPin?.connections?.length) {
      for (const conn of thenPin.connections) {
        lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, depth));
      }
    }
  }

  return lines;
}

export function describeGraph(graphData: any): string {
  const lines: string[] = [];
  const nodes: any[] = graphData.nodes || [];
  lines.push(`# ${graphData.name} (${nodes.length} nodes)`);

  // State machine description mode
  if (graphData.graphType === "StateMachine") {
    if (graphData.entryState) {
      lines.push(`Entry → ${graphData.entryState}`);
    }
    lines.push("");

    // Collect states and transitions
    const states = nodes.filter((n: any) => n.nodeType === "AnimState");
    const transitions = nodes.filter((n: any) => n.nodeType === "AnimTransition");

    if (states.length > 0) {
      lines.push("States:");
      for (const s of states) {
        const animInfo = s.animationAsset ? ` [AnimSequence: ${s.animationAsset}]` :
                         s.blendSpaceAsset ? ` [BlendSpace: ${s.blendSpaceAsset}]` : "";
        lines.push(`  ${s.stateName || s.title}${animInfo}`);
      }
    }

    if (transitions.length > 0) {
      lines.push("");
      lines.push("Transitions:");
      for (const t of transitions) {
        const from = t.fromState || "?";
        const to = t.toState || "?";
        const dur = t.crossfadeDuration !== undefined ? `${t.crossfadeDuration}s` : "?";
        const pri = t.priorityOrder !== undefined ? `priority ${t.priorityOrder}` : "";
        const bidir = t.bBidirectional ? ", bidirectional" : "";
        lines.push(`  ${from} → ${to} (${dur}${pri ? `, ${pri}` : ""}${bidir})`);
      }
    }

    return lines.join("\n");
  }

  // AnimGraph — identify anim node types
  if (graphData.graphType === "AnimGraph") {
    lines.push(`(Animation Graph)`);
  } else if (graphData.graphType === "TransitionRule") {
    lines.push(`(Transition Rule)`);
  }

  // Build node lookup
  const nodeMap: NodeMap = {};
  for (const n of nodes) {
    nodeMap[n.id] = n;
  }

  // Find entry points: Event nodes, CustomEvent nodes, FunctionEntry nodes
  const entryNodes = nodes.filter(
    (n: any) =>
      n.nodeType === "Event" ||
      n.nodeType === "CustomEvent" ||
      n.class?.includes("FunctionEntry") ||
      n.class?.includes("K2Node_Tunnel") && n.pins?.some((p: any) => p.type === "exec" && p.direction === "Output" && p.connections?.length)
  );

  if (entryNodes.length === 0) {
    // No entry points found - list all nodes as a fallback
    lines.push("\n(No event/entry nodes found)");
    lines.push("Nodes:");
    for (const n of nodes) {
      const desc = describeNode(n);
      if (desc) lines.push(`  ${desc}`);
    }
    return lines.join("\n");
  }

  for (const entry of entryNodes) {
    const label = entry.eventName || entry.title || entry.class;
    lines.push(`\n## on ${label}:`);

    // Find exec output pins to start walking
    const execOuts = (entry.pins || []).filter(
      (p: any) => p.type === "exec" && p.direction === "Output"
    );

    const visited = new Set<string>();
    visited.add(entry.id);

    for (const pin of execOuts) {
      if (pin.connections?.length) {
        for (const conn of pin.connections) {
          lines.push(...walkExecChain(conn.nodeId, nodeMap, visited, 0));
        }
      }
    }
  }

  return lines.join("\n");
}

export function summarizeBlueprint(data: any): string {
  const lines: string[] = [];
  lines.push(`# ${data.name}`);
  lines.push(`Parent: ${data.parentClass || "?"} | Path: ${data.path}`);
  if (data.blueprintType) lines.push(`Type: ${data.blueprintType}`);

  if (data.isAnimBlueprint) {
    lines.push(`Animation Blueprint: yes`);
    if (data.targetSkeleton) lines.push(`Target Skeleton: ${data.targetSkeleton}`);
  }

  if (data.interfaces?.length) {
    lines.push(`\n## Interfaces (${data.interfaces.length})`);
    for (const iface of data.interfaces) lines.push(`  ${iface}`);
  }

  if (data.variables?.length) {
    lines.push(`\n## Variables (${data.variables.length})`);
    for (const v of data.variables) {
      const defVal = v.defaultValue ? ` = ${v.defaultValue}` : "";
      const cat = v.category ? ` [${v.category}]` : "";
      const typeStr = flagType(formatVarType(v));
      lines.push(`  ${v.name}: ${typeStr}${defVal}${cat}`);
    }
  }

  if (data.graphs?.length) {
    lines.push(`\n## Graphs (${data.graphs.length})`);
    for (const g of data.graphs) {
      const nodes = g.nodes || [];
      const nodeCount = nodes.length;

      // Collect events with their parameters (#9)
      const events = nodes
        .filter((n: any) => n.nodeType === "Event" || n.nodeType === "CustomEvent")
        .map((n: any) => {
          const name = n.eventName || n.title;
          const params = (n.pins || [])
            .filter((p: any) => p.direction === "Output" && p.type !== "exec" && p.name !== "")
            .map((p: any) => ({ name: p.name, type: p.subtype || p.type || "" }));
          return `${name}${formatParams(params.length > 0 ? params : n.parameters)}`;
        });

      // Collect unique function calls
      const calls = [
        ...new Set(
          nodes
            .filter((n: any) => n.class?.includes("CallFunction") && n.functionName)
            .map((n: any) => n.functionName)
        ),
      ];

      // Collect variable writes
      const varSets = [
        ...new Set(
          nodes
            .filter((n: any) => n.nodeType === "VariableSet" && n.variableName)
            .map((n: any) => n.variableName)
        ),
      ];

      // Collect delegates with parameters (#9)
      const delegates = nodes
        .filter((n: any) => n.class?.includes("CreateDelegate") || n.class?.includes("DelegateFunction") || n.nodeType === "EventDispatcher")
        .map((n: any) => {
          const name = n.delegateName || n.functionName || n.title;
          return `${name}${formatParams(n.parameters)}`;
        });

      // Collect function entries with parameters (#9 - for function graphs)
      const funcEntries = nodes
        .filter((n: any) => n.class?.includes("FunctionEntry"))
        .map((n: any) => {
          const params = (n.pins || [])
            .filter((p: any) => p.direction === "Output" && p.type !== "exec" && p.name !== "")
            .map((p: any) => ({ name: p.name, type: p.subtype || p.type || "" }));
          return params.length > 0 ? formatParams(params) : "";
        });

      const funcParamStr = funcEntries.length === 1 ? funcEntries[0] : "";

      const graphTypeStr = g.graphType ? ` [${g.graphType}]` : "";
      lines.push(`  ${g.name} (${nodeCount} nodes)${funcParamStr}${graphTypeStr}`);
      if (events.length) lines.push(`    Events: ${events.join(", ")}`);
      if (delegates.length) lines.push(`    Delegates: ${delegates.join(", ")}`);
      if (calls.length) lines.push(`    Calls: ${calls.join(", ")}`);
      if (varSets.length) lines.push(`    Sets: ${varSets.join(", ")}`);
    }
  }

  return lines.join("\n");
}
