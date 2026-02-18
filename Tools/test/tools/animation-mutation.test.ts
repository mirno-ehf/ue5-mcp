import { describe, it, expect, beforeAll, afterAll } from "vitest";
import { uePost, ueGet, createTestAnimBlueprint, deleteTestBlueprint, uniqueName } from "../helpers.js";

describe("animation blueprint mutation tools", () => {
  const bpName = uniqueName("ABP_MutationTest");
  const packagePath = "/Game/Test";
  let stateMachineGraph: string;

  beforeAll(async () => {
    const result = await createTestAnimBlueprint({ name: bpName });
    expect(result.error).toBeUndefined();
    expect(result.success).toBe(true);

    // Add a state machine to get a state machine graph
    const smResult = await uePost("/api/add-state-machine", { blueprint: bpName });
    expect(smResult.error).toBeUndefined();
    stateMachineGraph = smResult.stateMachineGraph;
    expect(stateMachineGraph).toBeDefined();
  }, 60_000);

  afterAll(async () => {
    await deleteTestBlueprint(`${packagePath}/${bpName}`);
  }, 30_000);

  describe("create_anim_blueprint", () => {
    it("creates animation blueprint", async () => {
      const name = uniqueName("ABP_CreateTest");
      const result = await uePost("/api/create-anim-blueprint", {
        name,
        packagePath,
        skeleton: "__create_test_skeleton__",
      });
      expect(result.error).toBeUndefined();
      expect(result.success).toBe(true);
      expect(result.isAnimBlueprint).toBe(true);
      expect(result.targetSkeleton).toBeDefined();

      // Cleanup
      await deleteTestBlueprint(`${packagePath}/${name}`);
    });

    it("rejects missing required fields", async () => {
      const result = await uePost("/api/create-anim-blueprint", {});
      expect(result.error).toBeDefined();
    });

    it("rejects non-existent skeleton", async () => {
      const result = await uePost("/api/create-anim-blueprint", {
        name: uniqueName("ABP_BadSkel"),
        packagePath,
        skeleton: "NonExistentSkeleton_XYZ_999",
      });
      expect(result.error).toBeDefined();
    });
  });

  describe("add_anim_state", () => {
    it("adds a state to the state machine", async () => {
      const data = await uePost("/api/add-anim-state", {
        blueprint: bpName,
        graph: stateMachineGraph,
        stateName: "IdleState",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.stateName).toBe("IdleState");
      expect(data.nodeId).toBeDefined();
    });

    it("rejects duplicate state name", async () => {
      const data = await uePost("/api/add-anim-state", {
        blueprint: bpName,
        graph: stateMachineGraph,
        stateName: "IdleState",
      });
      expect(data.error).toBeDefined();
    });

    it("rejects non-ABP blueprint", async () => {
      const data = await uePost("/api/add-anim-state", {
        blueprint: "BP_NonexistentXYZ_999",
        graph: "SomeGraph",
        stateName: "Test",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("add_anim_transition", () => {
    beforeAll(async () => {
      // Add a second state so we can create a transition
      const data = await uePost("/api/add-anim-state", {
        blueprint: bpName,
        graph: stateMachineGraph,
        stateName: "RunState",
        posX: 400,
      });
      expect(data.error).toBeUndefined();
    });

    it("adds a transition between states", async () => {
      const data = await uePost("/api/add-anim-transition", {
        blueprint: bpName,
        graph: stateMachineGraph,
        fromState: "IdleState",
        toState: "RunState",
        crossfadeDuration: 0.25,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.fromState).toBe("IdleState");
      expect(data.toState).toBe("RunState");
      expect(data.nodeId).toBeDefined();
    });

    it("rejects non-existent states", async () => {
      const data = await uePost("/api/add-anim-transition", {
        blueprint: bpName,
        graph: stateMachineGraph,
        fromState: "NonExistentState",
        toState: "RunState",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("set_transition_rule", () => {
    it("updates transition properties", async () => {
      const data = await uePost("/api/set-transition-rule", {
        blueprint: bpName,
        graph: stateMachineGraph,
        fromState: "IdleState",
        toState: "RunState",
        crossfadeDuration: 0.5,
        priorityOrder: 2,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.crossfadeDuration).toBe(0.5);
      expect(data.priorityOrder).toBe(2);
    });

    it("rejects non-existent transition", async () => {
      const data = await uePost("/api/set-transition-rule", {
        blueprint: bpName,
        graph: stateMachineGraph,
        fromState: "RunState",
        toState: "IdleState",
        crossfadeDuration: 0.1,
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("remove_anim_state", () => {
    it("removes state and connected transitions", async () => {
      // Add a temporary state and transition
      await uePost("/api/add-anim-state", {
        blueprint: bpName,
        graph: stateMachineGraph,
        stateName: "TempState",
        posX: 600,
      });
      await uePost("/api/add-anim-transition", {
        blueprint: bpName,
        graph: stateMachineGraph,
        fromState: "IdleState",
        toState: "TempState",
      });

      const data = await uePost("/api/remove-anim-state", {
        blueprint: bpName,
        graph: stateMachineGraph,
        stateName: "TempState",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.removedState).toBe("TempState");
      expect(data.removedTransitions).toBeGreaterThanOrEqual(1);
    });
  });

  describe("add_anim_node", () => {
    it("adds a SequencePlayer node", async () => {
      const data = await uePost("/api/add-anim-node", {
        blueprint: bpName,
        nodeType: "SequencePlayer",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.nodeType).toBe("SequencePlayer");
      expect(data.nodeId).toBeDefined();
    });

    it("adds a StateMachine node with sub-graph", async () => {
      const data = await uePost("/api/add-anim-node", {
        blueprint: bpName,
        nodeType: "StateMachine",
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.stateMachineGraph).toBeDefined();
    });

    it("rejects unsupported nodeType", async () => {
      const data = await uePost("/api/add-anim-node", {
        blueprint: bpName,
        nodeType: "InvalidType",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("add_state_machine", () => {
    it("adds state machine to AnimGraph", async () => {
      const data = await uePost("/api/add-state-machine", {
        blueprint: bpName,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.nodeId).toBeDefined();
    });
  });

  describe("list_anim_slots", () => {
    it("returns slot list", async () => {
      const data = await uePost("/api/list-anim-slots", {
        blueprint: bpName,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.slots).toBeDefined();
      expect(Array.isArray(data.slots)).toBe(true);
    });

    it("rejects non-ABP", async () => {
      const data = await uePost("/api/list-anim-slots", {
        blueprint: "BP_NonexistentXYZ_999",
      });
      expect(data.error).toBeDefined();
    });
  });

  describe("list_sync_groups", () => {
    it("returns sync group list", async () => {
      const data = await uePost("/api/list-sync-groups", {
        blueprint: bpName,
      });
      expect(data.error).toBeUndefined();
      expect(data.success).toBe(true);
      expect(data.syncGroups).toBeDefined();
      expect(Array.isArray(data.syncGroups)).toBe(true);
    });
  });
});
