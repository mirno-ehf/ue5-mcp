import * as fs from "node:fs";
import * as path from "node:path";
import { spawn, type ChildProcess } from "node:child_process";

// --- Constants ---

export const UE_PROJECT_DIR = process.env.UE_PROJECT_DIR || process.cwd();
export const UE_PORT = parseInt(process.env.UE_PORT || "9847", 10);
export const UE_BASE_URL = `http://localhost:${UE_PORT}`;
export const PLUGIN_MODULE_NAME = "BlueprintMCP";
export const PLUGIN_DLL_NAME = `UnrealEditor-${PLUGIN_MODULE_NAME}.dll`;

// --- Mutable state singleton ---

export const state = {
  ueProcess: null as ChildProcess | null,
  editorMode: false,
  startupPromise: null as Promise<string | null> | null,
};

// --- UE5 Process Manager ---

/**
 * Read the EngineAssociation field from the .uproject file.
 * Returns a short version string like "5.4" or "5.7", or null.
 */
export function readEngineVersion(): string | null {
  const uproject = findUProject();
  if (!uproject) return null;
  try {
    const data = JSON.parse(fs.readFileSync(uproject, "utf-8"));
    if (typeof data.EngineAssociation === "string" && data.EngineAssociation) {
      return data.EngineAssociation;
    }
  } catch { /* ignore parse errors */ }
  return null;
}

export function findEditorCmd(): string | null {
  // Priority 1: explicit env var
  if (process.env.UE_EDITOR_CMD && fs.existsSync(process.env.UE_EDITOR_CMD)) {
    return process.env.UE_EDITOR_CMD;
  }

  // Priority 2: auto-detect from .uproject EngineAssociation
  const engineVersion = readEngineVersion();
  if (engineVersion) {
    const versionCandidates = [
      `C:\\Program Files\\Epic Games\\UE_${engineVersion}\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe`,
      `C:\\Program Files (x86)\\Epic Games\\UE_${engineVersion}\\Engine\\Binaries\\Win64\\UnrealEditor-Cmd.exe`,
    ];
    for (const p of versionCandidates) {
      if (fs.existsSync(p)) {
        console.error(`[BlueprintMCP] Auto-detected engine ${engineVersion} from .uproject`);
        return p;
      }
    }
  }

  // Priority 3: scan for any installed UE5 version
  const epicGamesDir = "C:\\Program Files\\Epic Games";
  try {
    const entries = fs.readdirSync(epicGamesDir);
    for (const entry of entries.sort().reverse()) {
      if (entry.startsWith("UE_")) {
        const candidate = path.join(epicGamesDir, entry, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe");
        if (fs.existsSync(candidate)) {
          const detectedVersion = entry.replace("UE_", "");
          console.error(`[BlueprintMCP] Found engine ${detectedVersion} (no match for .uproject version${engineVersion ? ` ${engineVersion}` : ""})`);
          return candidate;
        }
      }
    }
  } catch { /* directory may not exist */ }

  return null;
}

/** Find the .uproject file in UE_PROJECT_DIR (auto-detect by globbing). */
export function findUProject(): string | null {
  try {
    const entries = fs.readdirSync(UE_PROJECT_DIR);
    const uprojectFile = entries.find((e) => e.endsWith(".uproject"));
    if (uprojectFile) return path.join(UE_PROJECT_DIR, uprojectFile);
  } catch { /* ignore */ }
  return null;
}

/**
 * Ensure the .modules file in Binaries/Win64/ lists the BlueprintMCP module.
 * A build of only the Game target will overwrite this file without the editor module,
 * causing the commandlet to fail with "module could not be found".
 */
export function ensureModulesFile(): void {
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
export async function gracefulShutdown(): Promise<void> {
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
  if (state.ueProcess) {
    const proc = state.ueProcess;
    const exited = await new Promise<boolean>((resolve) => {
      const timer = setTimeout(() => resolve(false), 15000);
      proc.on("exit", () => { clearTimeout(timer); resolve(true); });
    });
    if (!exited && state.ueProcess) {
      console.error("[BlueprintMCP] Graceful shutdown timed out, force-killing.");
      state.ueProcess.kill();
    }
    state.ueProcess = null;
  }
}

/** Returns the health payload if the server is reachable, or null. */
export async function getUEHealth(): Promise<{ status: string; mode: string; blueprintCount: number; mapCount: number } | null> {
  try {
    const resp = await fetch(`${UE_BASE_URL}/api/health`, { signal: AbortSignal.timeout(2000) });
    if (!resp.ok) return null;
    return await resp.json() as any;
  } catch {
    return null;
  }
}

export async function isUEHealthy(): Promise<boolean> {
  return (await getUEHealth()) !== null;
}

export async function waitForHealthy(timeoutSeconds: number = 180): Promise<boolean> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  while (Date.now() < deadline) {
    if (await isUEHealthy()) return true;
    // If the process died while we were waiting, bail out
    if (!state.ueProcess) return false;
    await new Promise((r) => setTimeout(r, 2000));
  }
  return false;
}

export async function spawnAndWait(): Promise<string | null> {
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

  state.ueProcess = spawn(editorCmd, [
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

  state.ueProcess.on("exit", (code) => {
    console.error(`[BlueprintMCP] UE5 server exited with code ${code}`);
    state.ueProcess = null;
  });

  state.ueProcess.stdout?.on("data", (data: Buffer) => {
    console.error(`[UE5:out] ${data.toString().trim()}`);
  });
  state.ueProcess.stderr?.on("data", (data: Buffer) => {
    console.error(`[UE5:err] ${data.toString().trim()}`);
  });

  console.error("[BlueprintMCP] Waiting for health check (up to 3 min)...");
  const ok = await waitForHealthy(180);

  if (ok) {
    console.error("[BlueprintMCP] UE5 Blueprint server is ready.");
    return null; // success
  }

  // Failed — clean up
  if (state.ueProcess) {
    state.ueProcess.kill();
    state.ueProcess = null;
  }
  return "UE5 Blueprint server failed to start within 3 minutes. Check Saved/Logs/BlueprintMCP_server.log.";
}

export async function ensureUE(): Promise<string | null> {
  // Already healthy? Use the mode field to distinguish editor vs orphaned commandlet.
  const health = await getUEHealth();
  if (health) {
    state.editorMode = health.mode === "editor";
    return null;
  }

  state.editorMode = false;

  // If another call is already starting the server, wait on the same promise
  if (state.startupPromise) {
    return state.startupPromise;
  }

  // Kill stuck process if any
  if (state.ueProcess) {
    console.error("[BlueprintMCP] UE5 process exists but is not healthy. Killing and respawning...");
    state.ueProcess.kill();
    state.ueProcess = null;
  }

  // Ensure .modules file includes the plugin module before spawning
  ensureModulesFile();

  // Spawn and block until ready (shared promise prevents double-spawn)
  state.startupPromise = spawnAndWait().finally(() => { state.startupPromise = null; });
  return state.startupPromise;
}

// --- HTTP helpers ---

export async function ueGet(endpoint: string, params: Record<string, string> = {}): Promise<any> {
  const url = new URL(endpoint, UE_BASE_URL);
  for (const [k, v] of Object.entries(params)) {
    if (v) url.searchParams.set(k, v);
  }
  const resp = await fetch(url.toString(), { signal: AbortSignal.timeout(300000) }); // 5 min for search
  return resp.json();
}

export async function uePost(endpoint: string, body: Record<string, any>): Promise<any> {
  const resp = await fetch(`${UE_BASE_URL}${endpoint}`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(300000), // 5 min for compile+save
  });
  return resp.json();
}
