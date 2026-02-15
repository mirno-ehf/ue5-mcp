import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerComponentTools(server: McpServer): void {
  server.tool(
    "list_components",
    "List all components in a Blueprint's component hierarchy (Simple Construction Script). Shows component class, name, and parent-child relationships. Only works on Actor-based Blueprints.",
    {
      blueprint: z.string().describe("Blueprint name or package path (e.g. 'BP_Patient_Base')"),
    },
    async ({ blueprint }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/list-components", { blueprint });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Components (${data.count || 0}):`);

      for (const c of data.components || []) {
        const parent = c.parentComponent ? ` (parent: ${c.parentComponent})` : "";
        const root = c.isSceneRoot ? " [Root]" : "";
        const children = c.childCount > 0 ? ` [${c.childCount} children]` : "";
        lines.push(`  ${c.name}: ${c.componentClass}${parent}${root}${children}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "add_component",
    "Add a component to a Blueprint's component hierarchy (Simple Construction Script). Only works on Actor-based Blueprints. Common component classes: StaticMeshComponent, SkeletalMeshComponent, AudioComponent, SceneComponent, BoxCollisionComponent, SphereCollisionComponent, CapsuleComponent, ArrowComponent, ChildActorComponent, SpotLightComponent, PointLightComponent, WidgetComponent, BillboardComponent.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      componentClass: z.string().describe("Component class name (e.g. 'StaticMeshComponent', 'AudioComponent')"),
      name: z.string().describe("Name for the new component (e.g. 'MyMesh')"),
      parentComponent: z.string().optional().describe("Name of the parent component to attach to (optional, defaults to root set)"),
    },
    async ({ blueprint, componentClass, name, parentComponent }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { blueprint, componentClass, name };
      if (parentComponent) body.parentComponent = parentComponent;

      const data = await uePost("/api/add-component", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Component added successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Name: ${data.name}`);
      lines.push(`Class: ${data.componentClass}`);
      if (data.parentComponent) lines.push(`Parent: ${data.parentComponent}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_components(blueprint="${blueprint}") — verify the component hierarchy`);
      lines.push(`  set_blueprint_default(blueprint="${blueprint}", ...) — configure component properties`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "remove_component",
    "Remove a component from a Blueprint's component hierarchy (Simple Construction Script). Cannot remove a root component that has children — remove or re-parent children first. Children of non-root removed components are promoted to the removed component's parent.",
    {
      blueprint: z.string().describe("Blueprint name or package path"),
      name: z.string().describe("Name of the component to remove"),
    },
    async ({ blueprint, name }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/remove-component", { blueprint, name });
      if (data.error) {
        let msg = `Error: ${data.error}`;
        if (data.existingComponents?.length) {
          msg += `\nExisting components: ${data.existingComponents.join(", ")}`;
        }
        return { content: [{ type: "text" as const, text: msg }] };
      }

      const lines: string[] = [];
      lines.push(`Component removed successfully.`);
      lines.push(`Blueprint: ${data.blueprint}`);
      lines.push(`Removed: ${data.name}`);
      if (data.saved !== undefined) lines.push(`Saved: ${data.saved}`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_components(blueprint="${blueprint}") — verify the component was removed`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
