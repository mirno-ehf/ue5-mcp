import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

export function registerWorkflowRecipesResource(server: McpServer): void {
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
}
