import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet, uePost } from "../ue-bridge.js";

export function registerGroomTools(server: McpServer): void {
  // ------------------------------------------------------------------
  // list_groom_bindings
  // ------------------------------------------------------------------
  server.tool(
    "list_groom_bindings",
    "List all Groom Binding assets (UGroomBindingAsset) in the project. " +
    "Returns each binding's asset path, groom asset reference, target skeletal mesh, and source skeletal mesh. " +
    "Use the optional 'query' parameter to filter by name substring.",
    {
      query: z.string().optional().describe(
        "Optional name filter — returns only bindings whose name contains this string (case-insensitive)"
      ),
    },
    async ({ query }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const params: Record<string, string> = {};
      if (query) params.query = query;

      const data = await ueGet("/api/list-groom-bindings", params);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const bindings: any[] = data.bindings ?? [];
      if (bindings.length === 0) {
        const msg = query
          ? `No groom binding assets found matching '${query}'.`
          : "No groom binding assets found in the project.";
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [`Found ${data.count} groom binding(s):`];
      for (const b of bindings) {
        lines.push(`\n  ${b.name}`);
        lines.push(`    Asset path:   ${b.assetPath}`);
        if (b.groomAsset)         lines.push(`    Groom asset:  ${b.groomAsset}`);
        if (b.targetSkeletalMesh) lines.push(`    Target mesh:  ${b.targetSkeletalMesh}`);
        if (b.sourceSkeletalMesh) lines.push(`    Source mesh:  ${b.sourceSkeletalMesh}`);
      }

      lines.push(`\nNext steps:`);
      lines.push(`  • Use duplicate_groom_binding to copy a binding for a new character`);
      lines.push(`  • Use set_groom_binding_target_mesh to change which skeletal mesh a binding targets`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ------------------------------------------------------------------
  // duplicate_groom_binding
  // ------------------------------------------------------------------
  server.tool(
    "duplicate_groom_binding",
    "Duplicate a Groom Binding asset (.uasset) and give it a new name. " +
    "Useful when retargeting hair from one character to another — duplicate the original binding, " +
    "then call set_groom_binding_target_mesh on the copy to point it at the new skeletal mesh. " +
    "The copy retains the same groom (hair) asset reference as the original.",
    {
      assetPath: z.string().describe(
        "Full asset path or package path of the source groom binding " +
        "(e.g. '/Game/Characters/Hair/GB_Samir_Hair')"
      ),
      newName: z.string().describe(
        "Name for the duplicated asset — just the asset name, no slashes " +
        "(e.g. 'GB_Regina_Hair')"
      ),
      newFolder: z.string().optional().describe(
        "Destination package folder. Defaults to the same folder as the source. " +
        "Example: '/Game/Characters/Regina/Hair'"
      ),
    },
    async ({ assetPath, newName, newFolder }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { assetPath, newName };
      if (newFolder) body.newFolder = newFolder;

      const data = await uePost("/api/duplicate-groom-binding", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Groom binding duplicated successfully.`);
      lines.push(`  Original:  ${data.originalPath}`);
      lines.push(`  New asset: ${data.newPath}`);
      lines.push(`  Saved:     ${data.saved}`);
      if (data.warning) lines.push(`  Warning:   ${data.warning}`);

      lines.push(`\nNext steps:`);
      lines.push(`  1. Call set_groom_binding_target_mesh to point the new binding at the correct skeletal mesh`);
      lines.push(`  2. Open the binding in the Unreal Editor and click 'Rebuild Binding' to bake the new binding data`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  // ------------------------------------------------------------------
  // set_groom_binding_target_mesh
  // ------------------------------------------------------------------
  server.tool(
    "set_groom_binding_target_mesh",
    "Change the Target Skeletal Mesh (and optionally the Source Skeletal Mesh) reference inside a " +
    "Groom Binding asset. After calling this tool the binding's geometry data will be stale — " +
    "open the asset in the Unreal Editor and click 'Rebuild Binding' to regenerate the binding data " +
    "for the new mesh. Typical workflow: duplicate_groom_binding → set_groom_binding_target_mesh → rebuild in editor.",
    {
      assetPath: z.string().describe(
        "Full asset path or package path of the groom binding to modify " +
        "(e.g. '/Game/Characters/Hair/GB_Regina_Hair')"
      ),
      targetMeshPath: z.string().describe(
        "Full object path to the new target Skeletal Mesh asset " +
        "(e.g. '/Game/Characters/Regina/SKM_Regina.SKM_Regina')"
      ),
      sourceMeshPath: z.string().optional().describe(
        "Optional full object path to the source Skeletal Mesh (used for retargeting). " +
        "Pass this when the groom was originally created for a different skeleton than the target."
      ),
    },
    async ({ assetPath, targetMeshPath, sourceMeshPath }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { assetPath, targetMeshPath };
      if (sourceMeshPath) body.sourceMeshPath = sourceMeshPath;

      const data = await uePost("/api/set-groom-binding-target-mesh", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Groom binding target mesh updated.`);
      lines.push(`  Asset:           ${data.asset}`);
      lines.push(`  Old target mesh: ${data.oldTargetMesh || "(none)"}`);
      lines.push(`  New target mesh: ${data.newTargetMesh}`);
      if (data.oldSourceMesh !== undefined || data.newSourceMesh !== undefined) {
        lines.push(`  Old source mesh: ${data.oldSourceMesh || "(none)"}`);
        lines.push(`  New source mesh: ${data.newSourceMesh || "(unchanged)"}`);
      }
      lines.push(`  Saved:           ${data.saved}`);

      if (data.note) {
        lines.push(`\nIMPORTANT: ${data.note}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
