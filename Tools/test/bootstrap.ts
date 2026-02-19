/**
 * bootstrap.ts — Generate a temp UE5 project and manage the commandlet lifecycle.
 *
 * The temp project contains:
 *   TestProject.uproject  — minimal JSON pointing at BlueprintMCP plugin
 *   Plugins/BlueprintMCP/ — directory junction to the real plugin source
 *   Content/              — empty; tests create Blueprints into /Game/Test/
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as os from "node:os";
import { execSync, spawn, type ChildProcess } from "node:child_process";

/** Port used by the test commandlet (distinct from editor's 9847). */
export const TEST_PORT = 19847;
export const TEST_BASE_URL = `http://localhost:${TEST_PORT}`;

/** Absolute path to the plugin root (two levels up from test/). */
const PLUGIN_ROOT = path.resolve(import.meta.dirname, "..", "..");

let tempDir: string | null = null;
let cmdProcess: ChildProcess | null = null;

// ---------------------------------------------------------------------------
// Temp project generation
// ---------------------------------------------------------------------------

export function generateTempProject(): string {
  const dir = path.join(os.tmpdir(), `BlueprintMCP_Test_${Date.now()}`);
  fs.mkdirSync(dir, { recursive: true });

  // Minimal .uproject — engine version must match the compiled plugin DLL
  const engineVersion = detectEngineVersion();
  const uproject = {
    FileVersion: 3,
    EngineAssociation: engineVersion,
    Plugins: [{ Name: "BlueprintMCP", Enabled: true }],
  };
  fs.writeFileSync(
    path.join(dir, "TestProject.uproject"),
    JSON.stringify(uproject, null, "\t") + "\n",
  );

  // Content directory (blueprints land here)
  fs.mkdirSync(path.join(dir, "Content"), { recursive: true });

  // Plugins/BlueprintMCP → junction to real plugin
  const pluginsDir = path.join(dir, "Plugins");
  fs.mkdirSync(pluginsDir, { recursive: true });

  const junctionTarget = path.join(pluginsDir, "BlueprintMCP");
  // cmd /c mklink /J works without admin on Windows
  execSync(`cmd /c mklink /J "${junctionTarget}" "${PLUGIN_ROOT}"`, {
    stdio: "ignore",
  });

  tempDir = dir;
  console.log(`[bootstrap] Temp project created at ${dir}`);
  return dir;
}

// ---------------------------------------------------------------------------
// Commandlet lifecycle
// ---------------------------------------------------------------------------

/** Detect the UE engine version by scanning for installed engines (prefer newest). */
function detectEngineVersion(): string {
  const base = "C:\\Program Files\\Epic Games";
  try {
    const dirs = fs.readdirSync(base).filter((d) => d.startsWith("UE_"));
    // Sort descending so newest version is first
    dirs.sort((a, b) => b.localeCompare(a, undefined, { numeric: true }));
    if (dirs.length > 0) {
      return dirs[0].replace("UE_", "");
    }
  } catch { /* directory not found */ }
  return "5.4"; // fallback
}

function findEditorCmd(): string | null {
  if (process.env.UE_EDITOR_CMD && fs.existsSync(process.env.UE_EDITOR_CMD)) {
    return process.env.UE_EDITOR_CMD;
  }
  // Scan for installed UE versions, preferring newest
  const base = "C:\\Program Files\\Epic Games";
  const bases = [base, "C:\\Program Files (x86)\\Epic Games"];
  for (const b of bases) {
    try {
      const dirs = fs.readdirSync(b).filter((d) => d.startsWith("UE_"));
      dirs.sort((a, bv) => bv.localeCompare(a, undefined, { numeric: true }));
      for (const d of dirs) {
        const cmd = path.join(b, d, "Engine", "Binaries", "Win64", "UnrealEditor-Cmd.exe");
        if (fs.existsSync(cmd)) return cmd;
      }
    } catch { /* directory not found */ }
  }
  return null;
}

async function waitForHealth(timeoutMs: number = 240_000): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const resp = await fetch(`${TEST_BASE_URL}/api/health`, {
        signal: AbortSignal.timeout(2000),
      });
      if (resp.ok) return true;
    } catch {
      // not ready yet
    }
    if (!cmdProcess) return false; // process died
    await new Promise((r) => setTimeout(r, 2000));
  }
  return false;
}

export async function spawnCommandlet(projectDir: string): Promise<void> {
  const editorCmd = findEditorCmd();
  if (!editorCmd) {
    throw new Error(
      "UnrealEditor-Cmd.exe not found. Set UE_EDITOR_CMD env var.",
    );
  }

  const uproject = path.join(projectDir, "TestProject.uproject");
  const logPath = path.join(projectDir, "Saved", "Logs", "Test_server.log");

  console.log(`[bootstrap] Spawning commandlet on port ${TEST_PORT}...`);
  cmdProcess = spawn(
    editorCmd,
    [
      uproject,
      "-run=BlueprintMCP",
      `-port=${TEST_PORT}`,
      "-unattended",
      "-nopause",
      "-nullrhi",
      `-LOG=${logPath}`,
    ],
    { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
  );

  cmdProcess.stdout?.on("data", (d: Buffer) => {
    process.stderr.write(`[UE5:out] ${d.toString().trimEnd()}\n`);
  });
  cmdProcess.stderr?.on("data", (d: Buffer) => {
    process.stderr.write(`[UE5:err] ${d.toString().trimEnd()}\n`);
  });
  cmdProcess.on("exit", (code) => {
    console.log(`[bootstrap] Commandlet exited with code ${code}`);
    cmdProcess = null;
  });

  console.log("[bootstrap] Waiting for health (up to 4 min)...");
  const ok = await waitForHealth(240_000);
  if (!ok) {
    // Dump the log file if it exists
    try {
      const log = fs.readFileSync(logPath, "utf-8");
      console.error("[bootstrap] === Commandlet log (last 80 lines) ===");
      console.error(log.split("\n").slice(-80).join("\n"));
    } catch { /* no log */ }

    if (cmdProcess) {
      cmdProcess.kill();
      cmdProcess = null;
    }
    throw new Error("Commandlet failed to become healthy within 4 minutes.");
  }
  console.log("[bootstrap] Commandlet is healthy.");
}

// ---------------------------------------------------------------------------
// Shutdown & cleanup
// ---------------------------------------------------------------------------

export async function shutdownCommandlet(): Promise<void> {
  if (!cmdProcess) return;

  // Graceful HTTP shutdown
  try {
    await fetch(`${TEST_BASE_URL}/api/shutdown`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: "{}",
      signal: AbortSignal.timeout(3000),
    });
  } catch { /* may already be gone */ }

  // Wait for exit
  const proc = cmdProcess;
  const exited = await new Promise<boolean>((resolve) => {
    const timer = setTimeout(() => resolve(false), 15_000);
    proc.on("exit", () => {
      clearTimeout(timer);
      resolve(true);
    });
  });

  if (!exited && cmdProcess) {
    console.log("[bootstrap] Force-killing commandlet.");
    cmdProcess.kill();
    cmdProcess = null;
  }
}

export function cleanupTempProject(): void {
  if (!tempDir) return;
  const junctionPath = path.join(tempDir, "Plugins", "BlueprintMCP");

  // Remove the junction first — rmdir removes only the junction, not the target
  try {
    execSync(`cmd /c rmdir "${junctionPath}"`, { stdio: "ignore" });
  } catch { /* may already be gone */ }

  // Remove the rest of the temp directory
  try {
    fs.rmSync(tempDir, { recursive: true, force: true });
    console.log(`[bootstrap] Cleaned up ${tempDir}`);
  } catch (e) {
    console.error(`[bootstrap] Warning: could not clean up ${tempDir}:`, e);
  }
  tempDir = null;
}
