import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerLevelActorTools(server: McpServer): void {
  server.tool(
    "attach_actor",
    "Attach a child actor to a parent actor in the current level. Requires editor mode.",
    {
      childActor: z.string().describe("Label of the child actor to attach"),
      parentActor: z.string().describe("Label of the parent actor to attach to"),
      socketName: z.string().optional().describe("Optional socket name on the parent"),
      attachmentRule: z.enum(["KeepWorld", "KeepRelative", "SnapToTarget"]).optional()
        .describe("How to handle the child's transform on attach (default: KeepWorld)"),
    },
    async ({ childActor, parentActor, socketName, attachmentRule }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const body: Record<string, any> = { childActor, parentActor };
      if (socketName) body.socketName = socketName;
      if (attachmentRule) body.attachmentRule = attachmentRule;
      const data = await uePost("/api/attach-actor", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const lines = [
        `Attached '${data.childActor}' to '${data.parentActor}'`,
        `Attachment rule: ${data.attachmentRule}`,
      ];
      if (data.socketName) lines.push(`Socket: ${data.socketName}`);
      lines.push(`\nNext steps:`);
      lines.push(`  1. Use list_actors to verify the attachment hierarchy`);
      lines.push(`  2. Use detach_actor to undo the attachment`);
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "detach_actor",
    "Detach an actor from its parent in the current level. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor to detach"),
      detachmentRule: z.enum(["KeepWorld", "KeepRelative"]).optional()
        .describe("How to handle transform on detach (default: KeepWorld)"),
    },
    async ({ actorLabel, detachmentRule }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const body: Record<string, any> = { actorLabel };
      if (detachmentRule) body.detachmentRule = detachmentRule;
      const data = await uePost("/api/detach-actor", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      return { content: [{ type: "text" as const, text: `Detached '${data.actorLabel}' from '${data.previousParent}'\n\nNext steps:\n  1. Use list_actors to verify\n  2. Use set_actor_transform to reposition if needed` }] };
    }
  );

  server.tool(
    "duplicate_actor",
    "Duplicate an actor in the current level. Requires editor mode.",
    {
      actorLabel: z.string().describe("Label of the actor to duplicate"),
      newLabel: z.string().optional().describe("Optional label for the new actor"),
      offset: z.object({
        x: z.number().optional(),
        y: z.number().optional(),
        z: z.number().optional(),
      }).optional().describe("Optional position offset from the source actor"),
    },
    async ({ actorLabel, newLabel, offset }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const body: Record<string, any> = { actorLabel };
      if (newLabel) body.newLabel = newLabel;
      if (offset) body.offset = offset;
      const data = await uePost("/api/duplicate-actor", body);
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      const lines = [
        `Duplicated '${data.sourceActor}'`,
        `New actor: ${data.newActorLabel} (${data.newActorClass})`,
        `Location: (${data.location.x}, ${data.location.y}, ${data.location.z})`,
        `\nNext steps:`,
        `  1. Use set_actor_transform to reposition the duplicate`,
        `  2. Use rename_actor to give it a meaningful name`,
      ];
      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "rename_actor",
    "Rename an actor's label in the current level (World Outliner name). Requires editor mode.",
    {
      actorLabel: z.string().describe("Current label of the actor"),
      newLabel: z.string().describe("New label for the actor"),
    },
    async ({ actorLabel, newLabel }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };
      const data = await uePost("/api/rename-actor", { actorLabel, newLabel });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };
      return { content: [{ type: "text" as const, text: `Renamed '${data.oldLabel}' to '${data.newLabel}' (${data.actorClass})\n\nNext steps:\n  1. Use list_actors to verify` }] };
    }
  );
}
