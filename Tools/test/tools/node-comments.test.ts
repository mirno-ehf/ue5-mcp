import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, createTestBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("node_comments", () => {
  const bpName = uniqueName("BP_NodeCommentTest");
  const packagePath = "/Game/Test";
  let testNodeId: string;

  beforeAll(async () => {
    const res = await createTestBlueprint({ name: bpName });
    expect(res.error).toBeUndefined();

    // Add a node to test comments on
    const node = await uePost("/api/add-node", {
      blueprint: bpName,
      graph: "EventGraph",
      nodeType: "CallFunction",
      functionName: "PrintString",
    });
    expect(node.error).toBeUndefined();
    testNodeId = node.nodeId;
  });

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  });

  it("gets an empty comment on a new node", async () => {
    const data = await uePost("/api/get-node-comment", {
      blueprint: bpName,
      nodeId: testNodeId,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.comment).toBe("");
  });

  it("sets a comment on a node", async () => {
    const data = await uePost("/api/set-node-comment", {
      blueprint: bpName,
      nodeId: testNodeId,
      comment: "This prints a debug message",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.oldComment).toBe("");
    expect(data.newComment).toBe("This prints a debug message");
    expect(data.saved).toBe(true);
  });

  it("reads back the comment that was set", async () => {
    const data = await uePost("/api/get-node-comment", {
      blueprint: bpName,
      nodeId: testNodeId,
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.comment).toBe("This prints a debug message");
    expect(data.commentBubbleVisible).toBe(true);
  });

  it("clears a comment by setting empty string", async () => {
    const data = await uePost("/api/set-node-comment", {
      blueprint: bpName,
      nodeId: testNodeId,
      comment: "",
    });
    expect(data.error).toBeUndefined();
    expect(data.success).toBe(true);
    expect(data.oldComment).toBe("This prints a debug message");
    expect(data.newComment).toBe("");
    expect(data.saved).toBe(true);
  });

  it("rejects missing blueprint field", async () => {
    const data = await uePost("/api/set-node-comment", {
      nodeId: testNodeId,
      comment: "test",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing nodeId field", async () => {
    const data = await uePost("/api/set-node-comment", {
      blueprint: bpName,
      comment: "test",
    });
    expect(data.error).toBeDefined();
  });

  it("rejects missing comment field", async () => {
    const data = await uePost("/api/set-node-comment", {
      blueprint: bpName,
      nodeId: testNodeId,
    });
    expect(data.error).toBeDefined();
  });

  it("returns error for non-existent node", async () => {
    const data = await uePost("/api/set-node-comment", {
      blueprint: bpName,
      nodeId: "00000000-0000-0000-0000-000000000000",
      comment: "test",
    });
    expect(data.error).toBeDefined();
  });

  it("returns error for non-existent blueprint", async () => {
    const data = await uePost("/api/get-node-comment", {
      blueprint: "BP_Nonexistent_XYZ_999",
      nodeId: testNodeId,
    });
    expect(data.error).toBeDefined();
  });
});
