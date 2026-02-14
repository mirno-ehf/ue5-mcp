# BlueprintMCP — Claude Code Instructions

A UE5 editor plugin that exposes 32 MCP tools for inspecting and modifying Blueprint assets. Works with any UE5 5.4+ project.

Two serving modes:
- **Editor subsystem** (preferred): Auto-starts on port 9847 when the UE5 editor is open. Zero overhead.
- **Standalone commandlet**: Spawns a headless `UnrealEditor-Cmd.exe` process (2-4 GB RAM, ~60s startup). Call `shutdown_server` when done.

---

## Setup

Follow these steps sequentially when installing BlueprintMCP into a UE5 project for the first time.

### Prerequisites

| Requirement | Check command | Notes |
|-------------|--------------|-------|
| UE5 5.4+ | `ls "C:/Program Files/Epic Games/UE_5.4/"` | Uses Editor-only modules (`UnrealEd`, `BlueprintGraph`, `KismetCompiler`) |
| Node.js 18+ | `node --version` | Required for the TypeScript MCP server |
| npm | `npm --version` | Comes with Node.js |

### Step 1: Verify plugin location

The plugin must be inside the project's `Plugins/` directory:

```
<ProjectRoot>/
  <ProjectName>.uproject
  Plugins/
    BlueprintMCP/          <-- this repo
      BlueprintMCP.uplugin
      Source/
      Tools/
```

Verify from the project root:
```bash
ls Plugins/BlueprintMCP/BlueprintMCP.uplugin
```

Find the project root by locating the `.uproject` file:
```bash
ls *.uproject
```

### Step 2: Build the TypeScript MCP server

```bash
cd Plugins/BlueprintMCP/Tools
npm install
npm run build
```

Verify the build output exists:
```bash
ls Plugins/BlueprintMCP/Tools/dist/index.js
```

If `npm run build` fails, check that `tsconfig.json` exists and TypeScript is in `devDependencies`.

### Step 3: Create `.mcp.json` at the project root

Create or merge into `.mcp.json` in the directory containing the `.uproject` file:

```json
{
  "mcpServers": {
    "blueprint-mcp": {
      "command": "node",
      "args": ["Plugins/BlueprintMCP/Tools/dist/index.js"],
      "env": {
        "UE_PROJECT_DIR": "."
      }
    }
  }
}
```

If `.mcp.json` already exists, merge the `blueprint-mcp` key into the existing `mcpServers` object. Do not overwrite other servers.

**Important:** `.mcp.json` must be at the project root. Claude Code discovers it by searching the working directory and parent directories — it does not search subdirectories. Placing it inside `Plugins/BlueprintMCP/` would not work.

#### Environment variables

| Variable | Default | Description |
|----------|---------|-------------|
| `UE_PROJECT_DIR` | `process.cwd()` | Directory containing the `.uproject` file. Set to `"."` when `.mcp.json` is at project root. |
| `UE_PORT` | `9847` | HTTP port for the C++ backend. Change only if port 9847 is in use. |
| `UE_EDITOR_CMD` | Auto-detected | Full path to `UnrealEditor-Cmd.exe`. Only needed for commandlet mode if UE5 is in a non-standard location. |

#### Claude Desktop configuration

Claude Desktop uses absolute paths in `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "blueprint-mcp": {
      "command": "node",
      "args": ["C:/absolute/path/to/YourProject/Plugins/BlueprintMCP/Tools/dist/index.js"],
      "env": {
        "UE_PROJECT_DIR": "C:/absolute/path/to/YourProject"
      }
    }
  }
}
```

### Step 4: Build C++ (automatic)

The C++ plugin compiles automatically when the UE5 editor opens the project. No manual step is needed.

Optional pre-compile (replace project name and path):
```bash
"C:\Program Files\Epic Games\UE_5.4\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" YourProjectEditor Win64 Development -Project="C:\path\to\YourProject.uproject" -WaitMutex
```

### Step 5: Verify end-to-end

1. Open the UE5 project in the editor.
2. The editor subsystem auto-starts the HTTP server on port 9847.
3. Call the `server_status` tool. It should report the server is running in editor mode.

If the editor is not open, calling any tool will attempt to spawn a commandlet process.

---

## Development

Instructions for modifying BlueprintMCP's own source code (TypeScript or C++).

### Build requirements

**After ANY change to TypeScript or C++ files, you MUST build and verify before considering the work done.**

#### TypeScript

```bash
cd Plugins/BlueprintMCP/Tools
npm run build
```

Runs `tsc` and outputs to `dist/index.js`. The MCP server runs from `dist/`, not `src/` — changes to `src/index.ts` have no effect until built.

If the build fails with EPERM on dist files (Perforce read-only), clear the attribute first:
```bash
attrib -R "Plugins\BlueprintMCP\Tools\dist\index.js"
attrib -R "Plugins\BlueprintMCP\Tools\dist\index.js.map"
```

#### C++

Build from the project root:
```bash
"C:\Program Files\Epic Games\UE_5.4\Engine\Build\BatchFiles\Build.bat" <YourProject>Editor Win64 Development "<path\to\YourProject.uproject>" -waitmutex
```

Or open the `.sln` in Visual Studio and build **Development Editor | Win64** (Ctrl+Shift+B).

If C++ source files are read-only (Perforce), clear the attribute before editing:
```bash
attrib -R "Plugins\BlueprintMCP\Source\BlueprintMCP\Private\BlueprintMCPServer.cpp"
attrib -R "Plugins\BlueprintMCP\Source\BlueprintMCP\Public\BlueprintMCPServer.h"
```

### Architecture

```
src/index.ts (TypeScript MCP server)
    ↓ HTTP calls to localhost:9847
BlueprintMCPServer.cpp (C++ HTTP backend inside UE5)
    ↓ UE5 engine APIs
.uasset files
```

- **TypeScript** (`src/index.ts`): MCP tool definitions, response formatting, UE5 process lifecycle. Changes here affect tool schemas, descriptions, and the workflow recipes resource.
- **C++** (`BlueprintMCPServer.cpp`): Blueprint manipulation via UE5 APIs. Changes here affect what operations are possible and what data is returned.

### Key files

| File | Purpose |
|------|---------|
| `Tools/src/index.ts` | MCP server: tool definitions, response formatting, process management |
| `Source/BlueprintMCP/Public/BlueprintMCPServer.h` | C++ handler declarations |
| `Source/BlueprintMCP/Private/BlueprintMCPServer.cpp` | C++ HTTP endpoint implementations (~3700 lines) |
| `Source/BlueprintMCP/BlueprintMCP.Build.cs` | UE5 module dependencies |
| `Source/BlueprintMCP/Public/BlueprintMCPEditorSubsystem.h` | Editor subsystem header |
| `Source/BlueprintMCP/Private/BlueprintMCPEditorSubsystem.cpp` | Editor subsystem that hosts the server |
| `Source/BlueprintMCP/Public/BlueprintMCPCommandlet.h` | Standalone commandlet header |
| `Source/BlueprintMCP/Private/BlueprintMCPCommandlet.cpp` | Standalone commandlet for headless mode |

### Coding patterns: C++ handlers

Follow the existing pattern exactly:
- Parse JSON body with `ParseBodyJson()` or read query params
- Validate required fields, return `MakeErrorJson()` on failure
- Load blueprint with `LoadBlueprintByName()` (handles both regular BPs and level blueprints)
- Use SEH wrappers (`TryCompileBlueprintSEH`, `TrySavePackageSEH`) for crash safety on Windows
- Save with `SaveBlueprintPackage()` (handles compilation, map packages, and read-only files)
- Return JSON via `JsonToString()` with consistent field naming
- Log with `UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: ..."))`

### Coding patterns: TypeScript tools

Follow the existing pattern:
- Call `ensureUE()` first to guarantee the backend is running
- Use `ueGet()` for read-only endpoints, `uePost()` for mutations
- Format responses as human-readable text (not raw JSON)
- Include `nextSteps` hints for mutation tools
- Support `dryRun` parameter on mutation tools where applicable

### Adding a new tool

1. **C++ side:**
   - Declare handler method in `BlueprintMCPServer.h`
   - Implement in `BlueprintMCPServer.cpp` following the patterns above
   - Bind route in `Start()` method
   - Add dispatch case in `ProcessOneRequest()`

2. **TypeScript side:**
   - Add `server.tool(...)` definition in `src/index.ts` with Zod schema, HTTP call, and response formatting

3. **Build both:**
   - `cd Plugins/BlueprintMCP/Tools && npm run build`
   - Rebuild C++ via editor or build tool

### Testing

1. **Type check only** (fast): `npx tsc --noEmit` (from `Tools/`)
2. **Full build** (required): `npm run build` (from `Tools/`)
3. **Runtime test**: Open the UE5 editor, then call any blueprint-mcp tool — the editor subsystem auto-starts on port 9847
