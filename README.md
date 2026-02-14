# BlueprintMCP

An Unreal Engine 5 editor plugin that exposes Blueprint assets to AI coding assistants via the [Model Context Protocol (MCP)](https://modelcontextprotocol.io). Inspect, search, and modify Blueprint graphs programmatically from tools like Claude Code.

## Features

- Read and search Blueprint assets across your project
- Add, delete, and reconnect nodes in Blueprint graphs
- Change variable types, function parameters, and struct node types
- Snapshot and restore graph state for safe bulk edits
- Validate Blueprints and analyze C++ rebuild impact
- Works with both regular Blueprints and level Blueprints

## Prerequisites

- **Unreal Engine 5.4+**
- **Node.js 18+** (for the MCP server wrapper)
- **An MCP-compatible AI client** (e.g. Claude Code, Claude Desktop)
- **Windows** (macOS/Linux support possible but untested)

## Installation

### 1. Add the plugin to your UE5 project

Clone this repository into your project's `Plugins/` directory:

```bash
cd YourProject/Plugins
git clone https://github.com/Medagogic/ue5-mcp.git BlueprintMCP
```

Your project structure should look like:

```
YourProject/
  YourProject.uproject
  Plugins/
    BlueprintMCP/          <-- this repo
      BlueprintMCP.uplugin
      Source/
      Tools/
```

### 2. Build the C++ plugin

Open your `.uproject` file in the Unreal Editor. The plugin will compile automatically on first launch.

Alternatively, build from the command line:

```bash
"C:\Program Files\Epic Games\UE_5.4\Engine\Binaries\DotNET\UnrealBuildTool\UnrealBuildTool.exe" ^
  YourProjectEditor Win64 Development ^
  -Project="C:\path\to\YourProject.uproject" -WaitMutex
```

Verify the plugin is loaded: in the editor, go to **Edit > Plugins** and search for "Blueprint MCP".

### 3. Build the TypeScript MCP server

```bash
cd Plugins/BlueprintMCP/Tools
npm install
npm run build
```

This compiles `src/index.ts` to `dist/index.js`. The MCP server runs from `dist/`, not `src/`.

### 4. Configure your MCP client

Add a `.mcp.json` file to your **UE5 project root** (next to your `.uproject` file):

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

> **Why the project root?** Claude Code discovers `.mcp.json` by looking in the working directory and walking up parent directories. Placing it at the project root ensures it's found when you open Claude Code from anywhere within the project. The plugin ships with its own `.mcp.json` for standalone development, but it won't be discovered when working from the project root.

**Environment variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `UE_PROJECT_DIR` | Current working directory | Path to the directory containing your `.uproject` file |
| `UE_PORT` | `9847` | HTTP port for the C++ backend |
| `UE_EDITOR_CMD` | Auto-detected | Path to `UnrealEditor-Cmd.exe` (only needed for commandlet mode) |

For **Claude Desktop**, add to `claude_desktop_config.json`:

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

### 5. Verify the setup

1. Open your project in the Unreal Editor
2. Open Claude Code (or your MCP client) from the project directory
3. Use the `server_status` tool — it should report the editor subsystem is running on port 9847

## Serving Modes

### Editor mode (preferred)

When the UE5 editor is open, `UBlueprintMCPEditorSubsystem` automatically starts an HTTP server on port 9847. MCP tools connect instantly with zero startup time and no extra memory overhead.

### Commandlet fallback

When the editor is closed, the TypeScript wrapper spawns a standalone `UnrealEditor-Cmd.exe` commandlet process (~2-4 GB RAM, ~60s startup). Set the `UE_EDITOR_CMD` environment variable if auto-detection doesn't find your engine installation. Call the `shutdown_server` tool to free memory when done.

## Available Tools

### Reading & Searching

| Tool | Description |
|------|-------------|
| `list_blueprints` | List all Blueprint assets, optionally filtered by name or parent class |
| `get_blueprint` | Get full details: variables, graphs, nodes, and connections |
| `get_blueprint_graph` | Get a specific named graph (e.g. `EventGraph`, a function name) |
| `get_blueprint_summary` | Concise summary (~1-2K chars vs 300K+ raw JSON) |
| `describe_graph` | Pseudo-code description of control and data flow |
| `search_blueprints` | Search across Blueprints for matching nodes |
| `search_by_type` | Find all usages of a specific type across Blueprints |
| `find_asset_references` | Find all assets referencing a given asset path |

### Modifying

| Tool | Description |
|------|-------------|
| `add_node` | Add a node (BreakStruct, MakeStruct, CallFunction, etc.) |
| `delete_node` | Remove a node and disconnect all pins |
| `connect_pins` | Wire two pins together with type validation |
| `disconnect_pin` | Break connections on a pin |
| `set_pin_default` | Set the default value of an input pin |
| `set_blueprint_default` | Set a default property on a Blueprint's CDO |
| `change_variable_type` | Change a member variable's type |
| `change_function_parameter_type` | Change a function/event parameter's type |
| `change_struct_node_type` | Change a Break/Make struct node to a different struct |
| `remove_function_parameter` | Remove a parameter from a function or event |
| `replace_function_calls` | Redirect function calls from one library to another |
| `refresh_all_nodes` | Refresh all nodes after modifications |
| `rename_asset` | Rename or move a Blueprint and update references |
| `reparent_blueprint` | Change a Blueprint's parent class |
| `delete_asset` | Delete a .uasset file (checks for references first) |

### Validation & Safety

| Tool | Description |
|------|-------------|
| `validate_blueprint` | Compile and report errors/warnings without saving |
| `validate_all_blueprints` | Bulk-validate all (or filtered) Blueprints |
| `snapshot_graph` | Back up a graph's state before destructive operations |
| `diff_graph` | Compare current state against a snapshot |
| `restore_graph` | Reconnect severed pin connections from a snapshot |
| `find_disconnected_pins` | Scan for pins that should be connected but aren't |
| `analyze_rebuild_impact` | Predict which Blueprints a C++ rebuild will affect |

### Server Management

| Tool | Description |
|------|-------------|
| `server_status` | Check server status (starts if not running) |
| `shutdown_server` | Shut down the standalone commandlet to free memory |

## Architecture

```
MCP Client (Claude Code, Claude Desktop, etc.)
    |  MCP protocol (stdio)
    v
Tools/dist/index.js        (TypeScript — tool definitions, process management)
    |  HTTP to localhost:9847
    v
BlueprintMCPServer.cpp     (C++ — Blueprint manipulation via UE5 APIs)
    |
    v
Blueprint assets (.uasset)
```

The TypeScript layer defines MCP tool schemas, formats responses, and manages the UE5 process lifecycle. The C++ layer handles all actual Blueprint manipulation through the engine's APIs.

## License

[MIT](LICENSE)
