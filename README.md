# BlueprintMCP

Let AI coding assistants inspect and modify Unreal Engine 5 Blueprint assets via the [Model Context Protocol](https://modelcontextprotocol.io).

- 32 tools: read, search, add/delete/connect nodes, change types, snapshot/restore, validation
- Works with any UE5 5.4+ project — no game-specific dependencies
- **Editor mode:** zero-overhead subsystem when the editor is open
- **Commandlet mode:** headless standalone process when the editor is closed

## Quick Start

1. Clone into your project's `Plugins/BlueprintMCP/` directory
2. Build the TypeScript server: `cd Plugins/BlueprintMCP/Tools && npm install && npm run build`
3. Add to `.mcp.json` at your project root:
   ```json
   {
     "mcpServers": {
       "blueprint-mcp": {
         "command": "node",
         "args": ["Plugins/BlueprintMCP/Tools/dist/index.js"],
         "env": { "UE_PROJECT_DIR": "." }
       }
     }
   }
   ```
4. Open your project in the UE5 editor — the C++ plugin compiles and starts automatically

## Available Tools

| Tool | Description |
|------|-------------|
| `list_blueprints` | List Blueprint assets, optionally filtered by name or parent class |
| `get_blueprint` | Full details: variables, graphs, nodes, connections |
| `get_blueprint_graph` | Get a specific named graph (e.g. `EventGraph`, a function) |
| `get_blueprint_summary` | Concise summary (~1-2K chars vs 300K+ raw JSON) |
| `describe_graph` | Pseudo-code description of control and data flow |
| `search_blueprints` | Search across Blueprints for matching nodes |
| `search_by_type` | Find all usages of a specific type across Blueprints |
| `find_asset_references` | Find all assets referencing a given asset path |
| `add_node` | Add a node (function call, struct, variable, cast, etc.) |
| `delete_node` | Remove a node and disconnect all pins |
| `connect_pins` | Wire two pins together with type validation |
| `disconnect_pin` | Break connections on a pin |
| `set_pin_default` | Set the default value of an input pin |
| `set_blueprint_default` | Set a default property on a Blueprint's Class Default Object |
| `change_variable_type` | Change a member variable's type |
| `change_function_parameter_type` | Change a function/event parameter's type |
| `change_struct_node_type` | Change a Break/Make struct node to a different struct |
| `remove_function_parameter` | Remove a parameter from a function or event |
| `replace_function_calls` | Redirect function calls from one library to another |
| `refresh_all_nodes` | Refresh all nodes after modifications |
| `rename_asset` | Rename/move a Blueprint and update references |
| `reparent_blueprint` | Change a Blueprint's parent class |
| `delete_asset` | Delete a .uasset file (checks references first) |
| `validate_blueprint` | Compile and report errors/warnings without saving |
| `validate_all_blueprints` | Bulk-validate all (or filtered) Blueprints |
| `snapshot_graph` | Back up a graph's state before destructive operations |
| `diff_graph` | Compare current state against a snapshot |
| `restore_graph` | Reconnect severed pin connections from a snapshot |
| `find_disconnected_pins` | Scan for pins that should be connected but aren't |
| `analyze_rebuild_impact` | Predict which Blueprints a C++ rebuild will affect |
| `server_status` | Check server status (starts if not running) |
| `shutdown_server` | Shut down the standalone commandlet to free memory |

## Architecture

```
MCP Client (stdio) → Tools/dist/index.js (TypeScript) → HTTP :9847 → BlueprintMCPServer.cpp (C++) → .uasset files
```

## License

[MIT](LICENSE)
