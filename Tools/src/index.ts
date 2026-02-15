import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import * as fs from "node:fs";
import * as path from "node:path";
import { spawn, type ChildProcess } from "node:child_process";

const UE_PROJECT_DIR = process.env.UE_PROJECT_DIR || process.cwd();
const UE_PORT = parseInt(process.env.UE_PORT || "9847", 10);
const UE_BASE_URL = `http://localhost:${UE_PORT}`;

// --- Type name format documentation (shared across tool descriptions) ---

const TYPE_NAME_DOCS = `Type name formats: C++ USTRUCTs use F-prefixed name (e.g. 'FVitals', 'FDeviceState'), BP structs (UserDefinedStruct) use asset name (e.g. 'S_Vitals'), enums use enum name (e.g. 'ELungSound').`;

// --- Warning marker for unresolved types ---

const UNRESOLVED_TYPE_PATTERNS = ["<None>", "<unknown>", "None", "NONE"];

function flagType(typeName: string): string {
  if (!typeName) return "\u26A0 <None>";
  for (const pat of UNRESOLVED_TYPE_PATTERNS) {
    if (typeName === pat || typeName.includes(pat)) {
      return `\u26A0 ${typeName}`;
    }
  }
  return typeName;
}

// --- UE5 Process Manager ---

let ueProcess: ChildProcess | null = null;
let editorMode = false; // true when connected to a running UE5 editor (not a spawned commandlet)
let startupPromise: Promise<string | null> | null = null; // shared promise so concurrent calls don't double-spawn

function findEditorCmd(): string | null {
  // Check env var first
  if (process.env.UE_EDITOR_CMD && fs.existsSync(process.env.UE_EDITOR_CMD)) {
    return process.env.UE_EDITOR_CMD;
  }
  // Common install paths
  const candidates = [
    "C:\\Program Files\\Epic Games\\UE_5.4\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe",
    "C:\\Program Files (x86)\\Epic Games\\UE_5.4\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe",
  ];
  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return null;
}

// --- Module/DLL constants ---

const PLUGIN_MODULE_NAME = "BlueprintMCP";
const PLUGIN_DLL_NAME = `UnrealEditor-${PLUGIN_MODULE_NAME}.dll`;

/**
 * Ensure the .modules file in Binaries/Win64/ lists the BlueprintMCP module.
 * A build of only the Game target will overwrite this file without the editor module,
 * causing the commandlet to fail with "module could not be found".
 */
function ensureModulesFile(): void {
  const modulesPath = path.join(UE_PROJECT_DIR, "Binaries", "Win64", "UnrealEditor.modules");
  try {
    if (!fs.existsSync(modulesPath)) return; // nothing to fix
    const data = JSON.parse(fs.readFileSync(modulesPath, "utf-8"));
    if (data.Modules && !data.Modules[PLUGIN_MODULE_NAME]) {
      const dllPath = path.join(UE_PROJECT_DIR, "Binaries", "Win64", PLUGIN_DLL_NAME);
      if (!fs.existsSync(dllPath)) {
        console.error(`[BlueprintMCP] Warning: ${PLUGIN_DLL_NAME} not found — editor module may not be compiled.`);
        return;
      }
      data.Modules[PLUGIN_MODULE_NAME] = PLUGIN_DLL_NAME;
      fs.writeFileSync(modulesPath, JSON.stringify(data, null, "\t") + "\n", "utf-8");
      console.error(`[BlueprintMCP] Fixed .modules file — added ${PLUGIN_MODULE_NAME} entry.`);
    }
  } catch (e) {
    console.error("[BlueprintMCP] Warning: could not check/fix .modules file:", e);
  }
}

/**
 * Ask the UE5 server to shut down gracefully via /api/shutdown, then wait for
 * the process to exit. Falls back to kill after a timeout.
 */
async function gracefulShutdown(): Promise<void> {
  // Try graceful shutdown via HTTP
  try {
    await fetch(`${UE_BASE_URL}/api/shutdown`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
      signal: AbortSignal.timeout(3000),
    });
  } catch { /* server may already be gone */ }

  // Wait up to 15 seconds for process to exit on its own
  if (ueProcess) {
    const proc = ueProcess;
    const exited = await new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => resolve(false), 15000);
      proc.on("exit", () => { clearTimeout(timer); resolve(true); });
    });
    if (!exited && ueProcess) {
      console.error("[BlueprintMCP] Graceful shutdown timed out, force-killing.");
      ueProcess.kill();
    }
    ueProcess = null;
  }
}

/** Returns the health payload if the server is reachable, or null. */
async function getUEHealth(): Promise<{ status: string; mode: string; blueprintCount: number; mapCount: number } | null> {
  try {
    const resp = await fetch(`${UE_BASE_URL}/api/health`, { signal: AbortSignal.timeout(2000) });
    if (!resp.ok) return null;
    return await resp.json() as any;
  } catch {
    return null;
  }
}

async function isUEHealthy(): Promise<boolean> {
  return (await getUEHealth()) !== null;
}

async function waitForHealthy(timeoutSeconds: number = 180): Promise<boolean> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  while (Date.now() < deadline) {
    if (await isUEHealthy()) return true;
    // If the process died while we were waiting, bail out
    if (!ueProcess) return false;
    await new Promise((r) => setTimeout(r, 2000));
  }
  return false;
}

/** Find the .uproject file in UE_PROJECT_DIR (auto-detect by globbing). */
function findUProject(): string | null {
  try {
    const entries = fs.readdirSync(UE_PROJECT_DIR);
    const uprojectFile = entries.find((e) => e.endsWith(".uproject"));
    if (uprojectFile) return path.join(UE_PROJECT_DIR, uprojectFile);
  } catch { /* ignore */ }
  return null;
}

async function spawnAndWait(): Promise<string | null> {
  const editorCmd = findEditorCmd();
  if (!editorCmd) {
    return "Could not find UnrealEditor-Cmd.exe. Set UE_EDITOR_CMD environment variable.";
  }

  const uproject = findUProject();
  if (!uproject) {
    return `No .uproject file found in ${UE_PROJECT_DIR}`;
  }

  const logPath = path.join(UE_PROJECT_DIR, "Saved", "Logs", "BlueprintMCP_server.log");

  console.error("[BlueprintMCP] Spawning UE5 commandlet...");

  ueProcess = spawn(editorCmd, [
    uproject,
    "-run=BlueprintMCP",
    `-port=${UE_PORT}`,
    "-unattended",
    "-nopause",
    "-nullrhi",
    `-LOG=${logPath}`,
  ], {
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });

  ueProcess.on("exit", (code) => {
    console.error(`[BlueprintMCP] UE5 server exited with code ${code}`);
    ueProcess = null;
  });

  ueProcess.stdout?.on("data", (data: Buffer) => {
    console.error(`[UE5:out] ${data.toString().trim()}`);
  });
  ueProcess.stderr?.on("data", (data: Buffer) => {
    console.error(`[UE5:err] ${data.toString().trim()}`);
  });

  console.error("[BlueprintMCP] Waiting for health check (up to 3 min)...");
  const ok = await waitForHealthy(180);

  if (ok) {
    console.error("[BlueprintMCP] UE5 Blueprint server is ready.");
    return null; // success
  }

  // Failed — clean up
  if (ueProcess) {
    ueProcess.kill();
    ueProcess = null;
  }
  return "UE5 Blueprint server failed to start within 3 minutes. Check Saved/Logs/BlueprintMCP_server.log.";
}

async function ensureUE(): Promise<string | null> {
  // Already healthy? Use the mode field to distinguish editor vs orphaned commandlet.
  const health = await getUEHealth();
  if (health) {
    editorMode = health.mode === "editor";
    return null;
  }

  editorMode = false;

  // If another call is already starting the server, wait on the same promise
  if (startupPromise) {
    return startupPromise;
  }

  // Kill stuck process if any
  if (ueProcess) {
    console.error("[BlueprintMCP] UE5 process exists but is not healthy. Killing and respawning...");
    ueProcess.kill();
    ueProcess = null;
  }

  // Ensure .modules file includes the plugin module before spawning
  ensureModulesFile();

  // Spawn and block until ready (shared promise prevents double-spawn)
  startupPromise = spawnAndWait().finally(() => { startupPromise = null; });
  return startupPromise;
}

// Cleanup on exit — only kill the commandlet if we spawned it (don't kill the editor).
// The "exit" handler is synchronous so it can only do a hard kill as a last resort.
process.on("exit", () => { if (!editorMode) ueProcess?.kill(); });
// SIGINT/SIGTERM: attempt graceful shutdown, then exit.
for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, async () => {
    if (!editorMode && ueProcess) {
      await gracefulShutdown();
    }
    process.exit();
  });
}

// --- HTTP helpers ---

async function ueGet(endpoint: string, params: Record<string, string> = {}): Promise<any> {
  const url = new URL(endpoint, UE_BASE_URL);
  for (const [k, v] of Object.entries(params)) {
    if (v) url.searchParams.set(k, v);
  }
  const resp = await fetch(url.toString(), { signal: AbortSignal.timeout(300000) }); // 5 min for search
  return resp.json();
}

async function uePost(endpoint: string, body: Record<string, any>): Promise<any> {
  const resp = await fetch(`${UE_BASE_URL}${endpoint}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(300000), // 5 min for compile+save
  });
  return resp.json();
}

// --- Response formatting helpers ---

/** Format updated state returned by mutation tools (#11) */
function formatUpdatedState(data: any): string[] {
  const lines: string[] = [];
  if (data.updatedState) {
    lines.push(`\nUpdated state:`);
    const state = data.updatedState;
    if (state.variables?.length) {
      lines.push(`  Variables: ${state.variables.map((v: any) => `${v.name}: ${v.type}`).join(", ")}`);
    }
    if (state.pins?.length) {
      lines.push(`  Pins:`);
      for (const pin of state.pins) {
        lines.push(`    ${pin.direction === "Output" ? "\u2192" : "\u2190"} ${pin.name}: ${pin.type}${pin.subtype ? ` (${pin.subtype})` : ""}`);
      }
    }
    if (state.nodeCount !== undefined) {
      lines.push(`  Nodes: ${state.nodeCount}`);
    }
    if (state.graphCount !== undefined) {
      lines.push(`  Graphs: ${state.graphCount}`);
    }
  }
  return lines;
}

// --- Blueprint summary helpers ---

function formatVarType(v: any): string {
  let t = v.type || "unknown";
  if (v.subtype) t = v.subtype;
  if (v.isMap) return `Map<${v.type}, ${v.subtype || "?"}>`;
  if (v.isSet) return `Set<${t}>`;
  if (v.isArray) return `${t}[]`;
  return t;
}

/** Format parameter list for functions/events/delegates with type flagging (#9) */
function formatParams(params: any[] | undefined): string {
  if (!params || params.length === 0) return "";
  const parts = params.map((p: any) => {
    const name = p.name || "?";
    const type = p.type || p.pinType || "";
    return `${name}: ${flagType(type)}`;
  });
  return ` \u2014 Params: ${parts.join(", ")}`;
}

function summarizeBlueprint(data: any): string {
  const lines: string[] = [];
  lines.push(`# ${data.name}`);
  lines.push(`Parent: ${data.parentClass || "?"} | Path: ${data.path}`);
  if (data.blueprintType) lines.push(`Type: ${data.blueprintType}`);

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

      lines.push(`  ${g.name} (${nodeCount} nodes)${funcParamStr}`);
      if (events.length) lines.push(`    Events: ${events.join(", ")}`);
      if (delegates.length) lines.push(`    Delegates: ${delegates.join(", ")}`);
      if (calls.length) lines.push(`    Calls: ${calls.join(", ")}`);
      if (varSets.length) lines.push(`    Sets: ${varSets.join(", ")}`);
    }
  }

  return lines.join("\n");
}

interface NodeMap {
  [id: string]: any;
}

function describeNode(node: any): string {
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
function annotateDataFlow(node: any, nodeMap: NodeMap): string {
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
function annotateDataOutputs(node: any, nodeMap: NodeMap): string[] {
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

function walkExecChain(startNodeId: string, nodeMap: NodeMap, visited: Set<string>, depth: number = 0): string[] {
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

function describeGraph(graphData: any): string {
  const lines: string[] = [];
  const nodes: any[] = graphData.nodes || [];
  lines.push(`# ${graphData.name} (${nodes.length} nodes)`);

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

// --- MCP Server ---

const server = new McpServer({
  name: "blueprint-mcp",
  version: "1.0.0",
});

server.tool(
  "list_blueprints",
  "List all Blueprint assets in the UE5 project, including level blueprints from .umap files. Optionally filter by name/path substring or parent class.",
  {
    filter: z.string().optional().describe("Substring to match against Blueprint name or path"),
    parentClass: z.string().optional().describe("Filter by parent class name"),
  },
  async ({ filter, parentClass }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/list", {
      filter: filter || "",
      parentClass: parentClass || "",
    });

    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines = data.blueprints.map(
      (bp: any) => `${bp.name} (${bp.path}) [${bp.parentClass || "?"}]`
    );
    const summary = `Found ${data.count} of ${data.total} blueprints.\n\n${lines.join("\n")}`;
    return { content: [{ type: "text" as const, text: summary }] };
  }
);

server.tool(
  "get_blueprint",
  "Get full details of a specific Blueprint: variables, interfaces, and all graphs with nodes and connections. Also supports level blueprints from .umap files (e.g. 'MAP_Ward').",
  {
    name: z.string().describe("Blueprint name or package path (e.g. 'BP_Patient_Base', 'MAP_Ward')"),
  },
  async ({ name }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/blueprint", { name });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
  }
);

server.tool(
  "get_blueprint_graph",
  "Get a specific named graph from a Blueprint (e.g. 'EventGraph', a function name). Graph names are URL-encoded automatically.",
  {
    name: z.string().describe("Blueprint name or package path"),
    graph: z.string().describe("Graph name (e.g. 'EventGraph')"),
  },
  async ({ name, graph }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    // ueGet uses URL.searchParams.set which handles encoding via encodeURIComponent (#8)
    const data = await ueGet("/api/graph", { name, graph });
    if (data.error) {
      let msg = `Error: ${data.error}`;
      if (data.availableGraphs) msg += `\nAvailable: ${data.availableGraphs.join(", ")}`;
      return { content: [{ type: "text" as const, text: msg }] };
    }

    return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
  }
);

server.tool(
  "search_blueprints",
  "Search across Blueprints for nodes matching a query (function calls, events, variables). Loads BPs on demand so use 'path' filter to scope large searches.",
  {
    query: z.string().describe("Search term to match against node titles, function names, event names, variable names"),
    path: z.string().optional().describe("Filter to Blueprints whose path contains this substring (e.g. '/Game/Blueprints/Patients/')"),
    maxResults: z.number().optional().default(50).describe("Maximum results to return"),
  },
  async ({ query, path: pathFilter, maxResults }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/search", {
      query,
      path: pathFilter || "",
      maxResults: String(maxResults),
    });

    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines = data.results.map(
      (r: any) =>
        `[${r.blueprint}] ${r.graph} > ${r.nodeTitle}` +
        (r.functionName ? ` fn:${r.functionName}` : "") +
        (r.eventName ? ` event:${r.eventName}` : "") +
        (r.variableName ? ` var:${r.variableName}` : "")
    );
    const summary = `Found ${data.resultCount} results for "${query}":\n\n${lines.join("\n")}`;
    return { content: [{ type: "text" as const, text: summary }] };
  }
);

server.tool(
  "get_blueprint_summary",
  "Get a concise human-readable summary of a Blueprint: variables with types, graphs with node counts, events, and function calls. Returns ~1-2K chars instead of 300K+ raw JSON. Use this first to understand a Blueprint before diving into specific graphs.",
  {
    name: z.string().describe("Blueprint name or package path (e.g. 'BPC_3LeadECG')"),
  },
  async ({ name }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/blueprint", { name });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    return { content: [{ type: "text" as const, text: summarizeBlueprint(data) }] };
  }
);

server.tool(
  "describe_graph",
  "Get a pseudo-code description of a specific Blueprint graph by walking execution pin chains. Shows the control flow as readable pseudo-code (IF/CALL/SET/SEQUENCE etc) with data flow annotations showing where each node gets its inputs. Use after get_blueprint_summary to understand a specific graph's logic. Graph names are URL-encoded automatically.",
  {
    name: z.string().describe("Blueprint name or package path"),
    graph: z.string().describe("Graph name (e.g. 'EventGraph', 'Set Connection Progress')"),
  },
  async ({ name, graph }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    // ueGet uses URL.searchParams.set which handles encoding via encodeURIComponent (#8)
    const data = await ueGet("/api/graph", { name, graph });
    if (data.error) {
      let msg = `Error: ${data.error}`;
      if (data.availableGraphs) msg += `\nAvailable: ${data.availableGraphs.join(", ")}`;
      return { content: [{ type: "text" as const, text: msg }] };
    }

    return { content: [{ type: "text" as const, text: describeGraph(data) }] };
  }
);

server.tool(
  "find_asset_references",
  "Find all Blueprints (and other assets) that reference a given asset path. Equivalent to the editor's Reference Viewer. Use this to check dependencies before deleting assets or to map out which Blueprints use a specific struct, function library, or enum.",
  {
    assetPath: z.string().describe("Full asset path, e.g. '/Game/Blueprints/WebUI/S_Vitals'"),
  },
  async ({ assetPath }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/references", { assetPath });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`References to: ${data.assetPath}`);
    lines.push(`Total referencers: ${data.totalReferencers}`);

    if (data.blueprintReferencerCount > 0) {
      lines.push(`\nBlueprint referencers (${data.blueprintReferencerCount}):`);
      for (const ref of data.blueprintReferencers) {
        lines.push(`  ${ref}`);
      }
    }
    if (data.otherReferencerCount > 0) {
      lines.push(`\nOther referencers (${data.otherReferencerCount}):`);
      for (const ref of data.otherReferencers) {
        lines.push(`  ${ref}`);
      }
    }
    if (data.totalReferencers === 0) {
      lines.push("\nNo referencers found. Asset is safe to delete.");
    }

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

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

// --- New Tools ---

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
  "search_by_type",
  "Find all usages of a specific type across Blueprints: variables, function/event parameters, Break/Make struct nodes. More granular than find_asset_references.",
  {
    typeName: z.string().describe("Type name to search for (e.g. 'FVitals', 'S_Vitals', 'ELungSound')"),
    filter: z.string().optional().describe("Optional path filter to scope the search (e.g. '/Game/Blueprints/')"),
  },
  async ({ typeName, filter }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const params: Record<string, string> = { typeName };
    if (filter) params.filter = filter;

    const data = await ueGet("/api/search-by-type", params);
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Usages of type "${typeName}":`);

    if (data.variables?.length) {
      lines.push(`\nVariables (${data.variables.length}):`);
      for (const v of data.variables) {
        lines.push(`  ${v.blueprint}.${v.variableName}: ${v.type}`);
      }
    }

    if (data.parameters?.length) {
      lines.push(`\nFunction/Event Parameters (${data.parameters.length}):`);
      for (const p of data.parameters) {
        lines.push(`  ${p.blueprint}.${p.functionName}.${p.paramName}: ${p.type}`);
      }
    }

    if (data.structNodes?.length) {
      lines.push(`\nBreak/Make Struct Nodes (${data.structNodes.length}):`);
      for (const n of data.structNodes) {
        lines.push(`  ${n.blueprint} > ${n.graph} > ${n.nodeTitle} (${n.nodeId})`);
      }
    }

    if (data.otherUsages?.length) {
      lines.push(`\nOther Usages (${data.otherUsages.length}):`);
      for (const u of data.otherUsages) {
        lines.push(`  ${u.blueprint} > ${u.graph} > ${u.description}`);
      }
    }

    const total = (data.variables?.length || 0) + (data.parameters?.length || 0) +
      (data.structNodes?.length || 0) + (data.otherUsages?.length || 0);
    if (total === 0) {
      lines.push(`\nNo usages found.`);
    } else {
      lines.push(`\nTotal: ${total} usage(s)`);
    }

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "validate_blueprint",
  "Compile a Blueprint and report errors/warnings without saving. Captures both node-level compiler messages AND log-level messages (e.g. 'Can\\'t connect pins', 'Fixed up function'). Use after making changes to verify correctness.",
  {
    blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_PatientManager')"),
  },
  async ({ blueprint }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await uePost("/api/validate-blueprint", { blueprint });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Validation: ${data.blueprint}`);
    lines.push(`Status: ${data.status || (data.errors?.length ? "ERRORS" : "OK")}`);
    lines.push(`Valid: ${data.isValid}`);

    if (data.compileWarning) {
      lines.push(`\n\u26A0 ${data.compileWarning}`);
    }

    if (data.errors?.length) {
      lines.push(`\nErrors (${data.errorCount}):`);
      for (const e of data.errors) {
        if (typeof e === "string") {
          lines.push(`  \u2716 ${e}`);
        } else {
          const src = e.source === "log" ? "[log] " : "";
          const loc = e.graph ? `[${e.graph}] ` : "";
          const node = e.nodeTitle ? `${e.nodeTitle}: ` : "";
          lines.push(`  \u2716 ${src}${loc}${node}${e.message}`);
        }
      }
    }

    if (data.warnings?.length) {
      lines.push(`\nWarnings (${data.warningCount}):`);
      for (const w of data.warnings) {
        if (typeof w === "string") {
          lines.push(`  \u26A0 ${w}`);
        } else {
          const src = w.source === "log" ? "[log] " : "";
          const loc = w.graph ? `[${w.graph}] ` : "";
          const node = w.nodeTitle ? `${w.nodeTitle}: ` : "";
          lines.push(`  \u26A0 ${src}${loc}${node}${w.message}`);
        }
      }
    }

    if (!data.errors?.length && !data.warnings?.length) {
      lines.push(`\nNo errors or warnings. Blueprint compiles cleanly.`);
    }

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "validate_all_blueprints",
  "Bulk-validate all Blueprints (or a filtered subset) by compiling each one and reporting errors. Use after reparenting, C++ changes, or any operation that could cause cascading breakage. Returns only failed Blueprints to keep output manageable. Can take several minutes for large projects.",
  {
    filter: z.string().optional().describe("Optional path or name filter (e.g. '/Game/Blueprints/WebUI/' or 'HUD'). If omitted, validates ALL blueprints."),
  },
  async ({ filter }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const body: Record<string, string> = {};
    if (filter) body.filter = filter;

    const data = await uePost("/api/validate-all-blueprints", body);
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`# Bulk Validation Results`);
    if (data.filter) lines.push(`Filter: ${data.filter}`);
    lines.push(`Checked: ${data.totalChecked}`);
    lines.push(`Passed: ${data.totalPassed}`);
    lines.push(`Failed: ${data.totalFailed}`);
    if (data.totalCrashed) lines.push(`Crashed: ${data.totalCrashed}`);

    if (data.failed?.length) {
      lines.push(`\n## Failed Blueprints\n`);
      for (const bp of data.failed) {
        lines.push(`### ${bp.blueprint} (${bp.path || ""})`);
        lines.push(`Status: ${bp.status} | Errors: ${bp.errorCount} | Warnings: ${bp.warningCount}`);

        if (bp.errors?.length) {
          for (const e of bp.errors) {
            if (typeof e === "string") {
              lines.push(`  \u2716 ${e}`);
            } else {
              const src = e.source === "log" ? "[log] " : "";
              const loc = e.graph ? `[${e.graph}] ` : "";
              const node = e.nodeTitle ? `${e.nodeTitle}: ` : "";
              lines.push(`  \u2716 ${src}${loc}${node}${e.message}`);
            }
          }
        }

        if (bp.warnings?.length) {
          for (const w of bp.warnings) {
            if (typeof w === "string") {
              lines.push(`  \u26A0 ${w}`);
            } else {
              const src = w.source === "log" ? "[log] " : "";
              const loc = w.graph ? `[${w.graph}] ` : "";
              const node = w.nodeTitle ? `${w.nodeTitle}: ` : "";
              lines.push(`  \u26A0 ${src}${loc}${node}${w.message}`);
            }
          }
        }
        lines.push("");
      }
    } else {
      lines.push(`\nAll blueprints compile cleanly!`);
    }

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "add_node",
  "Add a new node to a Blueprint graph. Supports: BreakStruct, MakeStruct, CallFunction, VariableGet, VariableSet, DynamicCast, OverrideEvent, CallParentFunction.",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    graph: z.string().describe("Graph name (e.g. 'EventGraph')"),
    nodeType: z.enum(["BreakStruct", "MakeStruct", "CallFunction", "VariableGet", "VariableSet", "DynamicCast", "OverrideEvent", "CallParentFunction"]).describe("Type of node to add"),
    typeName: z.string().optional().describe("Struct type name for BreakStruct/MakeStruct (e.g. 'FVitals')"),
    functionName: z.string().optional().describe("Function name for CallFunction, OverrideEvent, or CallParentFunction (e.g. 'PrintString')"),
    className: z.string().optional().describe("Class name for CallFunction (e.g. 'KismetSystemLibrary')"),
    variableName: z.string().optional().describe("Variable name for VariableGet/VariableSet"),
    castTarget: z.string().optional().describe("Target class name for DynamicCast (e.g. 'BP_PatientJson')"),
    posX: z.number().optional().describe("X position in the graph (optional)"),
    posY: z.number().optional().describe("Y position in the graph (optional)"),
  },
  async ({ blueprint, graph, nodeType, typeName, functionName, className, variableName, castTarget, posX, posY }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const body: Record<string, any> = { blueprint, graph, nodeType };
    if (typeName) body.typeName = typeName;
    if (functionName) body.functionName = functionName;
    if (className) body.className = className;
    if (variableName) body.variableName = variableName;
    if (castTarget) body.castTarget = castTarget;
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
  "Rename or move a Blueprint asset and update all references.",
  {
    assetPath: z.string().describe("Current full asset path (e.g. '/Game/Blueprints/Old/BP_MyActor')"),
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
  "reparent_blueprint",
  "Change a Blueprint's parent class. Can reparent to a C++ class (e.g. 'WebUIHUD') or another Blueprint. Compiles, refreshes all nodes, and saves.",
  {
    blueprint: z.string().describe("Blueprint name or package path (e.g. 'HUD_WebUIInterface')"),
    newParentClass: z.string().describe("New parent class name — C++ class (e.g. 'WebUIHUD') or Blueprint name"),
  },
  async ({ blueprint, newParentClass }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await uePost("/api/reparent-blueprint", { blueprint, newParentClass });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Blueprint reparented successfully.`);
    lines.push(`Blueprint: ${data.blueprint}`);
    lines.push(`Old parent: ${data.oldParentClass}`);
    lines.push(`New parent: ${data.newParentClass}`);
    if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "create_blueprint",
  "Create a new Blueprint asset. Specify a parent class (C++ or Blueprint) and package path.",
  {
    blueprintName: z.string().describe("Name for the new Blueprint (e.g. 'BP_MyActor')"),
    packagePath: z.string().describe("Package path (e.g. '/Game/Blueprints/Actors')"),
    parentClass: z.string().describe("Parent class — C++ class (e.g. 'Actor', 'Pawn') or Blueprint name"),
    blueprintType: z.enum(["Normal", "Interface", "FunctionLibrary", "MacroLibrary"])
      .optional().default("Normal")
      .describe("Blueprint type (default: Normal)"),
  },
  async ({ blueprintName, packagePath, parentClass, blueprintType }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await uePost("/api/create-blueprint", { blueprintName, packagePath, parentClass, blueprintType });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Blueprint created successfully.`);
    lines.push(`Name: ${data.blueprintName}`);
    lines.push(`Path: ${data.assetPath}`);
    lines.push(`Parent: ${data.parentClass}`);
    lines.push(`Type: ${data.blueprintType}`);
    if (data.graphs?.length) lines.push(`Graphs: ${data.graphs.join(", ")}`);
    if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
    lines.push(``);
    lines.push(`Next steps:`);
    lines.push(`  get_blueprint(blueprint="${data.blueprintName}") — inspect the new Blueprint`);
    lines.push(`  add_node(blueprint="${data.blueprintName}", ...) — add logic`);

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
  "set_pin_default",
  "Set the default value of an input pin on a Blueprint node. Use this to set literal/constant values on pins that are not connected to other nodes (e.g. setting a String pin's default to 'SetSpeechData').",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    nodeId: z.string().describe("Node GUID (from get_blueprint_graph)"),
    pinName: z.string().describe("Pin name (e.g. 'Function', 'Value', 'Index')"),
    value: z.string().describe("Default value to set on the pin"),
  },
  async ({ blueprint, nodeId, pinName, value }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await uePost("/api/set-pin-default", { blueprint, nodeId, pinName, value });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Pin default set successfully.`);
    lines.push(`Blueprint: ${data.blueprint}`);
    lines.push(`Node: ${data.nodeId}`);
    lines.push(`Pin: ${data.pinName}`);
    lines.push(`Old value: ${data.oldValue || "(empty)"}`);
    lines.push(`New value: ${data.newValue}`);
    if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

// --- Graph creation ---

server.tool(
  "create_graph",
  "Create a new function graph, macro graph, or custom event in a Blueprint. For function/macro, creates a new named graph with entry/exit nodes. For customEvent, adds a CustomEvent node to the EventGraph.",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    graphName: z.string().describe("Name for the new graph or custom event"),
    graphType: z.enum(["function", "macro", "customEvent"]).describe("Type of graph to create: 'function' (new function graph), 'macro' (new macro graph), 'customEvent' (CustomEvent node in EventGraph)"),
  },
  async ({ blueprint, graphName, graphType }) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await uePost("/api/create-graph", { blueprint, graphName, graphType });
    if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

    const lines: string[] = [];
    lines.push(`Graph created successfully.`);
    lines.push(`Blueprint: ${data.blueprint}`);
    lines.push(`Graph: ${data.graphName}`);
    lines.push(`Type: ${data.graphType}`);
    if (data.nodeId) lines.push(`Node ID: ${data.nodeId}`);
    if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);
    lines.push(``);
    lines.push(`Next steps:`);
    if (graphType === "customEvent") {
      lines.push(`  add_node(blueprint="${blueprint}", graph="EventGraph", ...) — add logic after the event`);
    } else {
      lines.push(`  add_node(blueprint="${blueprint}", graph="${graphName}", ...) — add nodes to the new graph`);
    }
    lines.push(`  get_blueprint_graph(blueprint="${blueprint}", graph="${graphName}") — inspect the graph`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

// --- Variable management ---

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

// --- Existing utility tools ---

server.tool(
  "server_status",
  "Check UE5 Blueprint server status. Starts the server if not running (blocks until ready).",
  {},
  async () => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: err }] };

    const data = await ueGet("/api/health");
    return {
      content: [{
        type: "text" as const,
        text: `UE5 Blueprint server is running (${data.mode ?? (editorMode ? "editor" : "commandlet")} mode).\nBlueprints indexed: ${data.blueprintCount}\nMaps indexed: ${data.mapCount ?? "?"}`,
      }],
    };
  }
);

server.tool(
  "shutdown_server",
  "Shut down the UE5 Blueprint server to free memory (~2-4 GB). The server will auto-restart on the next blueprint tool call. Use this when done with blueprint analysis. Cannot shut down the editor — only the standalone commandlet.",
  {},
  async () => {
    if (editorMode) {
      return {
        content: [{
          type: "text" as const,
          text: "Connected to UE5 editor \u2014 cannot shut down the editor's MCP server. Close the editor to stop serving.",
        }],
      };
    }

    if (!ueProcess && !startupPromise && !(await isUEHealthy())) {
      return { content: [{ type: "text" as const, text: "UE5 server is already stopped." }] };
    }

    await gracefulShutdown();
    startupPromise = null;

    return {
      content: [{
        type: "text" as const,
        text: "UE5 Blueprint server shut down. Memory freed. It will auto-restart on the next blueprint tool call.",
      }],
    };
  }
);

// --- Resources ---

server.resource(
  "blueprint-list",
  "blueprint:///list",
  { description: "List of all exported Blueprints", mimeType: "application/json" },
  async (uri) => {
    if (!(await isUEHealthy())) {
      return { contents: [{ uri: uri.href, text: "[]", mimeType: "application/json" }] };
    }
    const data = await ueGet("/api/list");
    return { contents: [{ uri: uri.href, text: JSON.stringify(data.blueprints, null, 2), mimeType: "application/json" }] };
  }
);

// Workflow Recipes resource (#15)
server.resource(
  "workflow-recipes",
  "blueprint:///recipes",
  { description: "Workflow recipes for common Blueprint migration tasks", mimeType: "text/markdown" },
  async (uri) => {
    const recipes = `# Blueprint MCP Workflow Recipes

## Recipe 1: Migrate BP Struct to C++ USTRUCT

When replacing a Blueprint UserDefinedStruct (e.g. \`S_Vitals\`) with a C++ USTRUCT (e.g. \`FVitals\`):

### Steps

1. **Identify all usages** of the old struct:
   \`\`\`
   search_by_type(typeName="S_Vitals")
   find_asset_references(assetPath="/Game/Blueprints/WebUI/S_Vitals")
   \`\`\`

2. **Change variable types** in each Blueprint that declares a variable of this type:
   \`\`\`
   change_variable_type(blueprint="BP_PatientManager", variable="CurrentVitals", newType="FVitals", typeCategory="struct")
   \`\`\`

3. **Change function/event parameter types** where the struct is used as a parameter:
   \`\`\`
   change_function_parameter_type(blueprint="BP_PatientManager", functionName="UpdateVitals", paramName="Vitals", newType="FVitals")
   \`\`\`

4. **Update Break/Make struct nodes** to use the new type:
   \`\`\`
   change_struct_node_type(blueprint="BP_PatientJson", nodeId="<guid>", newType="FVitals")
   \`\`\`

5. **Refresh all nodes** in each modified Blueprint:
   \`\`\`
   refresh_all_nodes(blueprint="BP_PatientManager")
   \`\`\`

6. **Validate** each Blueprint compiles cleanly:
   \`\`\`
   validate_blueprint(blueprint="BP_PatientManager")
   \`\`\`

7. **Delete the old BP struct** once all references are removed:
   \`\`\`
   find_asset_references(assetPath="/Game/Blueprints/WebUI/S_Vitals")
   delete_asset(assetPath="/Game/Blueprints/WebUI/S_Vitals")
   \`\`\`

### Tips
- Use \`dryRun=true\` on mutation tools to preview changes first
- Use batch mode to change multiple variables/parameters at once
- The \`search_by_type\` tool is more granular than \`find_asset_references\` for finding specific usages
- After changing parameter types, check delegate graphs that bind to those functions

---

## Recipe 2: Convert BP Function Library to C++

When replacing a Blueprint Function Library (e.g. \`FL_StateParsers\`) with a C++ equivalent (e.g. \`UStateParsersLibrary\`):

### Steps

1. **Identify all Blueprints** that call functions from the library:
   \`\`\`
   find_asset_references(assetPath="/Game/Blueprints/WebUI/FL_StateParsers")
   \`\`\`

2. **For each referencing Blueprint**, redirect function calls:
   \`\`\`
   replace_function_calls(blueprint="BP_PatientJson", oldClass="FL_StateParsers", newClass="StateParsersLibrary")
   \`\`\`

3. **Refresh nodes** to update pin types:
   \`\`\`
   refresh_all_nodes(blueprint="BP_PatientJson")
   \`\`\`

4. **Fix broken connections** reported by replace_function_calls:
   - Use \`get_blueprint_graph\` to inspect the affected graph
   - Use \`connect_pins\` to rewire broken data connections
   - If pin types changed, use \`change_struct_node_type\` for Break/Make nodes

5. **Validate** each Blueprint:
   \`\`\`
   validate_blueprint(blueprint="BP_PatientJson")
   \`\`\`

6. **Delete the old BP function library**:
   \`\`\`
   delete_asset(assetPath="/Game/Blueprints/WebUI/FL_StateParsers")
   \`\`\`

### Tips
- Preview with \`dryRun=true\` on \`replace_function_calls\` first
- If function signatures changed (different parameter types), connections will break and need manual rewiring
- The \`brokenConnections\` array in the response tells you exactly which pins lost their wires

---

## Recipe 3: C++ Rebuild Safety

When rebuilding a C++ module containing USTRUCT/UENUM definitions:

### Before Rebuild
1. Analyze impact:
   \`analyze_rebuild_impact(moduleName="YourModule")\`

2. Snapshot HIGH-risk Blueprints:
   \`snapshot_graph(blueprint="BP_Affected1")\`
   \`snapshot_graph(blueprint="BP_Affected2")\`

### After Rebuild
3. Assess damage:
   \`find_disconnected_pins(filter="/Game/Blueprints/")\`

4. Diff each snapshot:
   \`diff_graph(blueprint="BP_Affected1", snapshotId="snap_...")\`

5. Fix broken struct types:
   \`change_struct_node_type(blueprint="BP_Affected1", nodeId="...", newType="FYourStruct")\`

6. Restore connections:
   \`restore_graph(blueprint="BP_Affected1", snapshotId="snap_...")\`

7. Verify:
   \`validate_blueprint(blueprint="BP_Affected1")\`
   \`find_disconnected_pins(blueprint="BP_Affected1")\`
`;
    return { contents: [{ uri: uri.href, text: recipes, mimeType: "text/markdown" }] };
  }
);

// --- Blueprint Safety Tools ---

server.tool(
  "snapshot_graph",
  "Create a backup snapshot of a Blueprint graph's state (all nodes, pins, and connections). Use BEFORE any destructive operation (C++ rebuild, change_struct_node_type, bulk edits). Returns a snapshot ID for later use with diff_graph or restore_graph. Snapshots are stored server-side and persist to disk.",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    graph: z.string().optional().describe("Specific graph name. If omitted, snapshots ALL graphs in the Blueprint"),
  },
  async (params) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

    const data = await uePost("/api/snapshot-graph", {
      blueprint: params.blueprint,
      graph: params.graph,
    });

    if (data.error) {
      return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
    }

    const lines: string[] = [];
    lines.push(`Snapshot created: ${data.snapshotId}`);
    lines.push(`Blueprint: ${data.blueprint}`);
    if (data.graphs) {
      const graphNames = data.graphs.map((g: any) => `${g.name} (${g.nodeCount} nodes, ${g.connectionCount} connections)`);
      lines.push(`Graphs: ${graphNames.join(", ")}`);
    }
    lines.push(`Total connections captured: ${data.totalConnections}`);
    lines.push(``);
    lines.push(`**Next steps:**`);
    lines.push(`1. Make your changes (C++ rebuild, change_struct_node_type, etc.)`);
    lines.push(`2. Run **diff_graph** to see what changed`);
    lines.push(`3. Run **restore_graph** to reconnect any severed pins`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "diff_graph",
  "Compare current Blueprint graph state against a snapshot. Shows severed connections, new connections, type changes, and missing nodes. Use AFTER a potentially destructive operation to assess damage before restoring.",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    snapshotId: z.string().describe("Snapshot ID from snapshot_graph"),
    graph: z.string().optional().describe("Specific graph to diff. If omitted, diffs all graphs in the snapshot"),
  },
  async (params) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

    const data = await uePost("/api/diff-graph", {
      blueprint: params.blueprint,
      snapshotId: params.snapshotId,
      graph: params.graph,
    });

    if (data.error) {
      return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
    }

    const lines: string[] = [];
    lines.push(`# Diff: ${data.blueprint} vs ${data.snapshotId}`);

    // Per-graph diffs
    if (data.graphDiffs) {
      for (const gd of data.graphDiffs) {
        lines.push(``);
        lines.push(`## ${gd.graphName}`);

        if (gd.severedConnections?.length) {
          lines.push(`  Severed connections (${gd.severedConnections.length}):`);
          for (const sc of gd.severedConnections) {
            lines.push(`    ${sc.sourceNodeTitle}.${sc.sourcePinName} was -> ${sc.targetNodeTitle}.${sc.targetPinName}`);
          }
        }

        if (gd.newConnections?.length) {
          lines.push(`  New connections (${gd.newConnections.length}):`);
          for (const nc of gd.newConnections) {
            lines.push(`    ${nc.sourceNodeTitle}.${nc.sourcePinName} -> ${nc.targetNodeTitle}.${nc.targetPinName}`);
          }
        }

        if (gd.typeChanges?.length) {
          lines.push(`  Type changes (${gd.typeChanges.length}):`);
          for (const tc of gd.typeChanges) {
            lines.push(`    Node ${tc.nodeId} (${tc.nodeTitle}): ${tc.oldType} -> ${tc.newType}`);
          }
        }

        if (gd.missingNodes?.length) {
          lines.push(`  Missing nodes (${gd.missingNodes.length}):`);
          for (const mn of gd.missingNodes) {
            lines.push(`    ${mn.nodeId} (${mn.nodeTitle}) - was ${mn.nodeClass}`);
          }
        }

        if (!gd.severedConnections?.length && !gd.newConnections?.length && !gd.typeChanges?.length && !gd.missingNodes?.length) {
          lines.push(`  No changes detected.`);
        }
      }
    }

    // Summary
    lines.push(``);
    if (data.summary) {
      lines.push(`Summary: ${data.summary.severed} severed, ${data.summary.new} new, ${data.summary.typeChanges} type changes, ${data.summary.missingNodes} missing nodes`);
    }

    // Next steps based on what was found
    lines.push(``);
    lines.push(`**Next steps:**`);
    if (data.summary?.typeChanges > 0) {
      lines.push(`1. Fix type changes first with **change_struct_node_type**`);
      lines.push(`2. Then run **restore_graph** to reconnect severed pins`);
    } else if (data.summary?.severed > 0) {
      lines.push(`1. Run **restore_graph** to reconnect severed pins`);
    } else {
      lines.push(`1. No action needed — graph is intact`);
    }
    lines.push(`3. Run **validate_blueprint** to verify clean compilation`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "restore_graph",
  "Reconnect severed pin connections from a snapshot. Use after diff_graph shows damage. Can restore an entire graph, a single node (nodeId), or use an explicit pin map. For Break/Make struct nodes that lost connections after change_struct_node_type or C++ rebuild, this bulk-reconnects all pins in one call instead of individual connect_pins calls.",
  {
    blueprint: z.string().describe("Blueprint name or package path"),
    snapshotId: z.string().describe("Snapshot ID from snapshot_graph"),
    graph: z.string().optional().describe("Specific graph to restore. If omitted, restores all graphs"),
    nodeId: z.string().optional().describe("Scope restore to a single node (e.g. a Break struct node). Useful after change_struct_node_type"),
    pinMap: z.record(z.string(), z.object({
      targetNodeId: z.string(),
      targetPinName: z.string(),
    })).optional().describe("Explicit pin mapping override: {outputPinName: {targetNodeId, targetPinName}}. Use when no snapshot exists or snapshot is stale"),
    dryRun: z.boolean().optional().describe("Preview reconnections without making changes"),
  },
  async (params) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

    const data = await uePost("/api/restore-graph", {
      blueprint: params.blueprint,
      snapshotId: params.snapshotId,
      graph: params.graph,
      nodeId: params.nodeId,
      pinMap: params.pinMap,
      dryRun: params.dryRun ?? false,
    });

    if (data.error) {
      return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
    }

    const lines: string[] = [];
    if (params.dryRun) {
      lines.push(`[DRY RUN - no changes will be made]\n`);
    }

    lines.push(`Restore: ${data.blueprint} from ${data.snapshotId}`);
    if (params.nodeId) {
      lines.push(`Scoped to node: ${params.nodeId}`);
    }
    lines.push(``);
    lines.push(`Reconnected: ${data.reconnected}/${data.reconnected + data.failed}`);
    if (data.failed > 0) {
      lines.push(`Failed: ${data.failed}`);
    }

    if (data.details?.length) {
      lines.push(``);
      lines.push(`Details:`);
      for (const d of data.details) {
        const status = d.result === "ok" ? "OK" : "FAILED";
        const reason = d.reason ? ` (${d.reason})` : "";
        lines.push(`  ${status}: ${d.sourcePinName} -> ${d.targetNodeTitle}.${d.targetPinName}${reason}`);
      }
    }

    if (!params.dryRun && data.saved !== undefined) {
      lines.push(``);
      lines.push(`Saved: ${data.saved}`);
    }

    lines.push(``);
    lines.push(`**Next steps:**`);
    if (data.failed > 0) {
      lines.push(`1. Fix ${data.failed} failed reconnection(s) manually with **connect_pins**`);
    }
    if (params.dryRun) {
      lines.push(`1. Re-run **restore_graph** without dryRun to apply changes`);
    }
    lines.push(`2. Run **validate_blueprint** to verify clean compilation`);
    lines.push(`3. Run **find_disconnected_pins** to verify no pins were missed`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "find_disconnected_pins",
  "Scan Blueprint(s) for pins that should be connected but aren't. Detects Break/Make struct nodes with broken types (HIGH confidence) or zero connections (MEDIUM confidence). Use after C++ rebuilds, change_struct_node_type, or refresh_all_nodes. Catches silent data flow breaks that validate_blueprint misses. Provide at least one of: blueprint, filter, or snapshotId.",
  {
    blueprint: z.string().optional().describe("Blueprint name or path. If omitted, scans multiple BPs using filter"),
    filter: z.string().optional().describe("Path filter when scanning multiple BPs (e.g. '/Game/Blueprints/Patients/'). Ignored when blueprint is specified"),
    snapshotId: z.string().optional().describe("Compare against snapshot for definite break detection. Without this, uses heuristics only"),
    sensitivity: z.enum(["high", "medium", "all"]).optional().describe("Detection sensitivity: 'high' = only broken types, 'medium' (default) = broken types + zero-connection Break nodes, 'all' = include partially connected Break nodes"),
  },
  async (params) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

    const data = await uePost("/api/find-disconnected-pins", {
      blueprint: params.blueprint,
      filter: params.filter,
      snapshotId: params.snapshotId,
      sensitivity: params.sensitivity ?? "medium",
    });

    if (data.error) {
      return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
    }

    const lines: string[] = [];

    if (!data.results || data.results.length === 0) {
      lines.push(`No disconnected pins found.`);
      if (data.summary) {
        lines.push(`Scanned ${data.summary.blueprintsScanned} Blueprint(s) — all Break/Make struct nodes have valid types and connected outputs.`);
      }
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }

    // Group by blueprint
    const byBP: Record<string, any[]> = {};
    for (const r of data.results) {
      const bp = r.blueprint || params.blueprint || "unknown";
      if (!byBP[bp]) byBP[bp] = [];
      byBP[bp].push(r);
    }

    lines.push(`Disconnected pins found:\n`);

    for (const [bp, results] of Object.entries(byBP)) {
      lines.push(`## ${bp}`);
      for (const r of results) {
        const conf = r.confidence === "high" ? "HIGH" : r.confidence === "medium" ? "MEDIUM" : "LOW";
        const icon = r.confidence === "high" ? "\u26A0" : r.confidence === "medium" ? "\u26A1" : "\u2139";
        lines.push(`  ${icon} ${conf} — ${r.nodeTitle || "BreakStruct"} (${r.nodeId}) in ${r.graph}`);
        lines.push(`    Type: ${flagType(r.structType || "")}`);
        lines.push(`    Reason: ${r.reason}`);
        if (r.pins?.length) {
          for (const p of r.pins) {
            const was = p.wasConnectedTo ? ` (was -> ${p.wasConnectedTo})` : "";
            lines.push(`      ${p.name}: ${p.type} — disconnected${was}`);
          }
        }
      }
      lines.push(``);
    }

    if (data.summary) {
      lines.push(`Summary: ${data.summary.high} HIGH, ${data.summary.medium} MEDIUM across ${data.summary.blueprintsScanned} Blueprint(s)`);
    }

    lines.push(``);
    lines.push(`**Next steps:**`);
    if (data.summary?.high > 0) {
      lines.push(`1. Fix HIGH-confidence issues: use **change_struct_node_type** to restore struct types`);
    }
    lines.push(`2. Use **restore_graph** (if snapshot exists) to bulk-reconnect severed pins`);
    lines.push(`3. Or use **connect_pins** for individual reconnections`);
    lines.push(`4. Run **validate_blueprint** to verify compilation`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

server.tool(
  "analyze_rebuild_impact",
  "Predict which Blueprints will be affected by a C++ module rebuild. Scans for Break/Make struct nodes, variables, and function parameters that reference USTRUCTs/UENUMs defined in the specified module. Use BEFORE rebuilding to know what to snapshot. " + TYPE_NAME_DOCS,
  {
    moduleName: z.string().describe("C++ module name (e.g. 'MyGame'). Finds all Blueprints using types from this module"),
    structNames: z.array(z.string()).optional().describe("Specific struct/enum names to check (e.g. ['FVitals', 'FSkinState']). If omitted, checks ALL types from the module"),
  },
  async (params) => {
    const err = await ensureUE();
    if (err) return { content: [{ type: "text" as const, text: `Error: ${err}` }] };

    const data = await uePost("/api/analyze-rebuild-impact", {
      moduleName: params.moduleName,
      structNames: params.structNames,
    });

    if (data.error) {
      return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
    }

    const lines: string[] = [];
    lines.push(`# Rebuild Impact Analysis: ${data.moduleName}`);
    lines.push(``);

    if (data.typesFound?.length) {
      lines.push(`Types in module (${data.typesFound.length}): ${data.typesFound.join(", ")}`);
      lines.push(``);
    }

    if (!data.affectedBlueprints?.length) {
      lines.push(`No Blueprints reference types from this module.`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }

    lines.push(`Affected Blueprints (${data.affectedBlueprints.length}):\n`);

    for (const bp of data.affectedBlueprints) {
      const risk = bp.risk || "UNKNOWN";
      lines.push(`  **${bp.name}** (${risk} risk)`);
      if (bp.breakNodes > 0) lines.push(`    Break nodes: ${bp.breakNodes}${bp.breakNodeTypes ? ` (${bp.breakNodeTypes.join(", ")})` : ""}`);
      if (bp.makeNodes > 0) lines.push(`    Make nodes: ${bp.makeNodes}${bp.makeNodeTypes ? ` (${bp.makeNodeTypes.join(", ")})` : ""}`);
      if (bp.variables > 0) lines.push(`    Variables: ${bp.variables}`);
      if (bp.functionParams > 0) lines.push(`    Function params: ${bp.functionParams}`);
      if (bp.connectionsAtRisk > 0) lines.push(`    Connections at risk: ~${bp.connectionsAtRisk}`);
      lines.push(``);
    }

    if (data.summary) {
      lines.push(`Total: ${data.summary.totalBlueprints} Blueprints, ${data.summary.totalBreakMakeNodes} Break/Make nodes, ~${data.summary.totalConnectionsAtRisk} connections at risk`);
    }

    lines.push(``);
    lines.push(`**Next steps:**`);
    lines.push(`1. Run **snapshot_graph** on each HIGH-risk Blueprint BEFORE rebuilding:`);
    const highRisk = (data.affectedBlueprints || []).filter((bp: any) => bp.risk === "HIGH");
    for (const bp of highRisk.slice(0, 5)) {
      lines.push(`   snapshot_graph(blueprint="${bp.name}")`);
    }
    if (highRisk.length > 5) {
      lines.push(`   ... and ${highRisk.length - 5} more`);
    }
    lines.push(`2. After rebuild, run **find_disconnected_pins** to assess damage`);
    lines.push(`3. Use **restore_graph** on each Blueprint to reconnect severed pins`);

    return { content: [{ type: "text" as const, text: lines.join("\n") }] };
  }
);

// --- Start ---

async function main() {
  // Try to connect to a running UE5 server
  const health = await getUEHealth();
  if (health) {
    editorMode = health.mode === "editor";
    console.error(`Connected to UE5 ${health.mode} \u2014 MCP server already running.`);
  } else {
    editorMode = false;
    console.error("UE5 server not detected. Commandlet will be spawned on first tool call.");
  }

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
