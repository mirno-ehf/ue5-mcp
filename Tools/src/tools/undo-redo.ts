import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { ensureUE, uePost } from "../ue-bridge.js";

export function registerUndoRedoTools(server: McpServer): void {
  server.tool(
    "undo",
    "Undo the last editor action. Returns the description of the undone action and remaining undo/redo counts. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/undo", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Undone: ${data.undoneAction || "(unnamed action)"}`,
        `Remaining undo actions: ${data.remainingUndoCount}`,
        `Available redo actions: ${data.redoCount}`,
        `\nNext steps:`,
        `  1. Use redo to re-apply the undone action`,
        `  2. Use undo again to undo further`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "redo",
    "Redo the last undone editor action. Returns the description of the redone action and remaining undo/redo counts. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/redo", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Redone: ${data.redoneAction || "(unnamed action)"}`,
        `Available undo actions: ${data.undoCount}`,
        `Remaining redo actions: ${data.remainingRedoCount}`,
        `\nNext steps:`,
        `  1. Use undo to undo the redone action`,
        `  2. Use redo again to redo further`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "begin_transaction",
    "Begin a named undo transaction. All modifications between begin_transaction and end_transaction will be grouped as a single undoable action. Requires editor mode.",
    {
      description: z.string().describe("Human-readable description of the transaction (shown in Edit > Undo)"),
    },
    async ({ description }) => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/begin-transaction", { description });
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Transaction started: '${data.description}'`,
        `Transaction index: ${data.transactionIndex}`,
        `\nNext steps:`,
        `  1. Make your modifications (set_actor_transform, set_actor_property, etc.)`,
        `  2. Call end_transaction to close the transaction`,
        `  3. The entire group can then be undone with a single undo call`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );

  server.tool(
    "end_transaction",
    "End the current undo transaction. All modifications since the matching begin_transaction will be grouped as a single undoable action. Requires editor mode.",
    {},
    async () => {
      const err = await ensureUE();
      if (err) return { content: [{ type: "text" as const, text: err }] };

      const data = await uePost("/api/end-transaction", {});
      if (data.error) return { content: [{ type: "text" as const, text: `Error: ${data.error}` }] };

      const lines = [
        `Transaction ended`,
        `Transaction index: ${data.transactionIndex}`,
        `\nNext steps:`,
        `  1. Use undo to undo the entire transaction as one action`,
      ];

      return { content: [{ type: "text" as const, text: lines.join("\n") }] };
    }
  );
}
