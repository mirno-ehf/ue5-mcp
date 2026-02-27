import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, ueGet, uePost } from "../ue-bridge.js";

export function registerLevelTools(server: McpServer): void {
  server.tool(
    "get_current_level",
    "Get information about the currently open level in the Unreal Editor, including its name, package path, and actor count.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await ueGet("/api/current-level", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Level: ${data.levelName}`);
      lines.push(`Package: ${data.packageName}`);
      lines.push(`Actor count: ${data.actorCount}`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "list_actors",
    "List actors placed in the currently open level. Supports optional filtering by class name, actor label, or outliner folder. Returns label, class, folder, and location for each actor.",
    {
      classFilter: z.string().optional().describe("Substring filter on class name (e.g. 'StaticMesh', 'Light', 'Camera')"),
      nameFilter:  z.string().optional().describe("Substring filter on actor label (display name in world outliner)"),
      folder:      z.string().optional().describe("Outliner folder prefix to filter by (e.g. 'Lights', 'Environment/Props')"),
    },
    async ({ classFilter, nameFilter, folder }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const params: Record<string, string> = {};
      if (classFilter) params.classFilter = classFilter;
      if (nameFilter)  params.nameFilter  = nameFilter;
      if (folder)      params.folder      = folder;

      const data = await ueGet("/api/list-actors", params);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Level: ${data.level}`);
      lines.push(`Actors (${data.count || 0}):`);

      for (const a of data.actors || []) {
        const loc = a.location ? ` @ (${a.location.x?.toFixed(1)}, ${a.location.y?.toFixed(1)}, ${a.location.z?.toFixed(1)})` : "";
        const folder = a.folder ? ` [${a.folder}]` : "";
        lines.push(`  ${a.label}: ${a.class}${loc}${folder}`);
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "get_actor_properties",
    "Get all editable (CPF_Edit) properties of a placed actor in the current level, identified by its display label. Returns property name, C++ type, current value, and whether the value is at its class default. Complex struct properties are automatically expanded into individual sub-fields so no values are silently omitted. Pass 'component' to inspect a specific component's properties (e.g. 'StaticMeshComponent0'). Without 'component', also returns a 'components' list for discovery.",
    {
      label:     z.string().describe("Actor display label as shown in the world outliner (case-insensitive)"),
      component: z.string().optional().describe("Component name to inspect (e.g. 'StaticMeshComponent0'). Omit to list actor-level properties and discover available components."),
    },
    async ({ label, component }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const params: Record<string, string> = { label };
      if (component) params.component = component;

      const data = await ueGet("/api/actor-properties", params);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];

      const formatProps = (props: any[]) => {
        let lastStruct = "";
        for (const p of props) {
          const def = p.isDefault ? " (default)" : "";
          if (p.struct) {
            // Struct sub-field: group under struct header
            if (p.struct !== lastStruct) {
              lines.push(`  [${p.struct}]`);
              lastStruct = p.struct;
            }
            lines.push(`    ${p.name} [${p.type}] = ${p.value}${def}`);
          } else {
            lastStruct = "";
            lines.push(`  ${p.name} [${p.type}] = ${p.value}${def}`);
          }
        }
      };

      if (component) {
        lines.push(`Actor: ${data.label} — Component: ${data.component} (${data.class})`);
        lines.push(`Properties (${data.count || 0}):`);
        formatProps(data.properties || []);
        lines.push(``);
        lines.push(`Tip: set_actor_property(label="${label}", property="${component}.PropertyName", value="...")`);
      } else {
        lines.push(`Actor: ${data.label} (${data.class})`);
        lines.push(`Properties (${data.count || 0}):`);
        formatProps(data.properties || []);
        if (data.components && data.components.length > 0) {
          lines.push(``);
          lines.push(`Components (${data.components.length}):`);
          for (const c of data.components) {
            lines.push(`  ${c.name} (${c.class})`);
          }
          lines.push(``);
          lines.push(`Tip: get_actor_properties(label="${label}", component="<name>") to inspect component properties`);
          lines.push(`     set_actor_property(label="${label}", property="ComponentName.PropName", value="...") to set them`);
        }
      }

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_actor_transform",
    "Move, rotate, and/or scale a placed actor in the current level. All fields are optional — only provided fields are applied. Location/scale are in centimeters; rotation is in degrees (pitch/yaw/roll).",
    {
      label:    z.string().describe("Actor display label (case-insensitive)"),
      location: z.object({
        x: z.number().optional(),
        y: z.number().optional(),
        z: z.number().optional(),
      }).optional().describe("New world location in cm (partial updates supported)"),
      rotation: z.object({
        pitch: z.number().optional(),
        yaw:   z.number().optional(),
        roll:  z.number().optional(),
      }).optional().describe("New world rotation in degrees (partial updates supported)"),
      scale: z.object({
        x: z.number().optional(),
        y: z.number().optional(),
        z: z.number().optional(),
      }).optional().describe("New world scale (partial updates supported)"),
    },
    async ({ label, location, rotation, scale }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { label };
      if (location) body.location = location;
      if (rotation) body.rotation = rotation;
      if (scale)    body.scale    = scale;

      const data = await uePost("/api/set-actor-transform", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const loc = data.location;
      const rot = data.rotation;
      const scl = data.scale;

      const lines: string[] = [];
      lines.push(`Transform updated: ${data.label}`);
      if (loc) lines.push(`  Location: (${loc.x?.toFixed(2)}, ${loc.y?.toFixed(2)}, ${loc.z?.toFixed(2)}) cm`);
      if (rot) lines.push(`  Rotation: pitch=${rot.pitch?.toFixed(2)} yaw=${rot.yaw?.toFixed(2)} roll=${rot.roll?.toFixed(2)}`);
      if (scl) lines.push(`  Scale:    (${scl.x?.toFixed(3)}, ${scl.y?.toFixed(3)}, ${scl.z?.toFixed(3)})`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_actors(nameFilter="${label}") — verify the new position`);
      lines.push(`  get_actor_properties(label="${label}") — inspect other properties`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "set_actor_property",
    "Set a named property on a placed actor using UE5 reflection. Supports actor-level properties ('bHidden', 'Mobility') and component sub-properties using dot notation ('StaticMeshComponent0.StaticMesh'). Values use UE import-text format (e.g. '1.0', 'true', '/Engine/BasicShapes/Cube.Cube').",
    {
      label:    z.string().describe("Actor display label (case-insensitive)"),
      property: z.string().describe("C++ property name on the actor ('bHidden') or component sub-property ('StaticMeshComponent0.StaticMesh')"),
      value:    z.string().describe("Value in UE text-import format (e.g. '1.0', 'true', '/Engine/BasicShapes/Cube.Cube', '(R=1,G=0,B=0,A=1)')"),
    },
    async ({ label, property, value }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/set-actor-property", { label, property, value });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Property set successfully.`);
      lines.push(`Actor:    ${data.label}`);
      if (data.component) lines.push(`Component: ${data.component}`);
      lines.push(`Property: ${data.property}`);
      lines.push(`Value:    ${data.value}`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  get_actor_properties(label="${label}") — verify all properties`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "spawn_actor",
    "Spawn a new actor in the currently open level. The class can be a C++ class name (e.g. 'StaticMeshActor', 'PointLight', 'DirectionalLight') or a Blueprint class name (e.g. 'BP_MyActor'). Location defaults to world origin if not specified.",
    {
      class:    z.string().describe("Actor class name — C++ (e.g. 'StaticMeshActor') or Blueprint (e.g. 'BP_MyActor')"),
      label:    z.string().optional().describe("Display label for the new actor in the world outliner"),
      location: z.object({
        x: z.number().optional().default(0),
        y: z.number().optional().default(0),
        z: z.number().optional().default(0),
      }).optional().describe("Spawn location in world space (cm)"),
      rotation: z.object({
        pitch: z.number().optional().default(0),
        yaw:   z.number().optional().default(0),
        roll:  z.number().optional().default(0),
      }).optional().describe("Spawn rotation in degrees"),
      folder: z.string().optional().describe("Outliner folder path to place the actor in (e.g. 'Enemies/Ground')"),
    },
    async ({ class: actorClass, label, location, rotation, folder }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const body: Record<string, any> = { class: actorClass };
      if (label)    body.label    = label;
      if (location) body.location = location;
      if (rotation) body.rotation = rotation;
      if (folder)   body.folder   = folder;

      const data = await uePost("/api/spawn-actor", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const loc = data.location;

      const lines: string[] = [];
      lines.push(`Actor spawned successfully.`);
      lines.push(`Label: ${data.label}`);
      lines.push(`Class: ${data.class}`);
      if (loc) lines.push(`Location: (${loc.x?.toFixed(1)}, ${loc.y?.toFixed(1)}, ${loc.z?.toFixed(1)}) cm`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  set_actor_property(label="${data.label}", ...) — configure the actor`);
      lines.push(`  set_actor_transform(label="${data.label}", ...) — reposition the actor`);
      lines.push(`  delete_actor(label="${data.label}") — remove it if no longer needed`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "delete_actor",
    "Delete a placed actor from the current level by its display label. This operation is undoable (Ctrl+Z in the editor). The actor must exist in the level.",
    {
      label: z.string().describe("Actor display label to delete (case-insensitive)"),
    },
    async ({ label }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/delete-actor", { label });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines: string[] = [];
      lines.push(`Actor deleted successfully.`);
      lines.push(`Label: ${data.label}`);
      lines.push(`Class: ${data.class}`);

      lines.push(``);
      lines.push(`Next steps:`);
      lines.push(`  list_actors() — verify the actor was removed`);
      lines.push(`  (Ctrl+Z in the editor to undo)`);

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
