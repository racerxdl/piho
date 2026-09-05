import { describe, expect, test } from "bun:test";
import { join } from "node:path";
import { compileFlowDocument, type CompilationResult } from "../src/compile.ts";
import { decodeGraphImage } from "../src/image.ts";
import { inspectGraphImage } from "../src/inspect.ts";
import { parseStrictJson } from "../src/json.ts";

type MutableSource = {
  format: number;
  executorApi: number;
  generation: number;
  devices: Record<string, unknown>;
  inputs: Record<string, unknown>;
  actions: Record<string, unknown>;
  flows: Record<string, unknown>;
};

function validSource(): MutableSource {
  return {
    format: 1,
    executorApi: 1,
    generation: 9,
    devices: {
      input_board: { id: 1, role: "input" },
      output_board: { id: 7, role: "output" },
    },
    inputs: {
      button: { device: "input_board", pin: 2, debounceMs: 25 },
    },
    actions: {
      relay: { device: "output_board", pin: 3, operation: "toggle" },
    },
    flows: {
      primary: {
        when: [{ input: "button", edge: "rising" }],
        actions: ["relay"],
      },
    },
  };
}

const examplePath = join(import.meta.dir, "../examples/synthetic.json");
const goldenPath = join(import.meta.dir, "fixtures/synthetic.phg");

async function compileExample(): Promise<CompilationResult> {
  return compileFlowDocument(parseStrictJson(await Bun.file(examplePath).text()));
}

describe("canonical compiler", () => {
  test("ignores declaration, trigger, and action-reference ordering", () => {
    const first = {
      format: 1,
      executorApi: 1,
      generation: 12,
      devices: {
        source: { id: 1, role: "input" },
        target: { id: 7, role: "output" },
      },
      inputs: {
        alpha: { device: "source", pin: 1, debounceMs: 20 },
        beta: { device: "source", pin: 2, debounceMs: 30 },
      },
      actions: {
        enable: { device: "target", pin: 4, operation: "set", value: true },
        toggle: { device: "target", pin: 5, operation: "toggle" },
      },
      flows: {
        route: {
          when: [
            { input: "beta", edge: "falling" },
            { input: "alpha", edge: "rising" },
          ],
          actions: ["toggle", "enable"],
        },
      },
    };
    const reordered = {
      flows: {
        route: {
          actions: ["enable", "toggle"],
          when: [
            { edge: "rising", input: "alpha" },
            { edge: "falling", input: "beta" },
          ],
        },
      },
      actions: {
        toggle: { operation: "toggle", pin: 5, device: "target" },
        enable: { value: true, operation: "set", pin: 4, device: "target" },
      },
      inputs: {
        beta: { debounceMs: 30, pin: 2, device: "source" },
        alpha: { debounceMs: 20, pin: 1, device: "source" },
      },
      devices: {
        target: { role: "output", id: 7 },
        source: { role: "input", id: 1 },
      },
      generation: 12,
      executorApi: 1,
      format: 1,
    };

    expect(compileFlowDocument(first).image).toEqual(compileFlowDocument(reordered).image);
  });

  test("matches the complete golden image and decodes every table", async () => {
    const { image } = await compileExample();
    expect(image).toEqual(new Uint8Array(await Bun.file(goldenPath).arrayBuffer()));
    expect(decodeGraphImage(image)).toEqual({
      format: 1,
      executorApi: 1,
      generation: 1,
      flowCount: 3,
      devices: [
        {
          id: 1,
          role: "input",
          inputStart: 0,
          inputCount: 1,
          routeStart: 0,
          routeCount: 2,
          actionStart: 0,
          actionCount: 0,
        },
        {
          id: 2,
          role: "input",
          inputStart: 1,
          inputCount: 1,
          routeStart: 2,
          routeCount: 1,
          actionStart: 0,
          actionCount: 0,
        },
        {
          id: 7,
          role: "output",
          inputStart: 0,
          inputCount: 0,
          routeStart: 0,
          routeCount: 0,
          actionStart: 0,
          actionCount: 3,
        },
        {
          id: 8,
          role: "output",
          inputStart: 0,
          inputCount: 0,
          routeStart: 0,
          routeCount: 0,
          actionStart: 3,
          actionCount: 1,
        },
      ],
      inputs: [
        { id: 1, device: 1, pin: 2, debounceMs: 25 },
        { id: 2, device: 2, pin: 5, debounceMs: 40 },
      ],
      routes: [
        { id: 2, flowId: 2, inputId: 1, ownerDevice: 1, edge: "rising", actionIds: [1, 4] },
        { id: 1, flowId: 1, inputId: 1, ownerDevice: 1, edge: "falling", actionIds: [3] },
        { id: 3, flowId: 3, inputId: 2, ownerDevice: 2, edge: "changed", actionIds: [2] },
      ],
      actions: [
        {
          id: 2,
          targetDevice: 7,
          targetPin: 6,
          operation: "copy_source",
          value: false,
          delayMs: 0,
          durationMs: 0,
        },
        {
          id: 3,
          targetDevice: 7,
          targetPin: 9,
          operation: "pulse",
          value: false,
          delayMs: 250,
          durationMs: 1000,
        },
        {
          id: 4,
          targetDevice: 7,
          targetPin: 3,
          operation: "toggle",
          value: false,
          delayMs: 0,
          durationMs: 0,
        },
        {
          id: 1,
          targetDevice: 8,
          targetPin: 4,
          operation: "set",
          value: true,
          delayMs: 60000,
          durationMs: 0,
        },
      ],
    });
  });

  test("reports manifest, counts, offsets, sizes, and checksum", async () => {
    const report = inspectGraphImage((await compileExample()).image);
    expect(report).toEqual({
      magic: "PHGF",
      format: 1,
      executorApi: 1,
      generation: 1,
      imageBytes: 244,
      crc32: "0xae85369a",
      counts: {
        devices: 4,
        inputs: 2,
        flows: 3,
        routes: 3,
        actionReferences: 4,
        actions: 4,
      },
      sections: {
        header: { offset: 0, bytes: 56, records: 1 },
        devices: { offset: 56, bytes: 64, records: 4 },
        inputs: { offset: 120, bytes: 16, records: 2 },
        routes: { offset: 136, bytes: 36, records: 3 },
        actionReferences: { offset: 172, bytes: 8, records: 4 },
        actions: { offset: 180, bytes: 64, records: 4 },
      },
      devices: [
        {
          id: 1,
          role: "input",
          inputs: { start: 0, count: 1 },
          routes: { start: 0, count: 2 },
          actions: { start: 0, count: 0 },
        },
        {
          id: 2,
          role: "input",
          inputs: { start: 1, count: 1 },
          routes: { start: 2, count: 1 },
          actions: { start: 0, count: 0 },
        },
        {
          id: 7,
          role: "output",
          inputs: { start: 0, count: 0 },
          routes: { start: 0, count: 0 },
          actions: { start: 0, count: 3 },
        },
        {
          id: 8,
          role: "output",
          inputs: { start: 0, count: 0 },
          routes: { start: 0, count: 0 },
          actions: { start: 3, count: 1 },
        },
      ],
    });
  });

  test("detects graph image corruption", async () => {
    const corrupt = (await compileExample()).image.slice();
    corrupt[60] = (corrupt[60] ?? 0) ^ 0x01;
    expect(() => decodeGraphImage(corrupt)).toThrow("graph image checksum mismatch");
  });
});

describe("strict validation", () => {
  test("rejects duplicate JSON keys", () => {
    expect(() => parseStrictJson('{"format":1,"format":1}')).toThrow(
      "$.format: is a duplicate JSON key",
    );
  });

  test("rejects invalid IDs, roles, pins, references, cycles, operators, and targets", () => {
    const invalidId = validSource();
    invalidId.devices.input_board = { id: 32, role: "input" };
    expect(() => compileFlowDocument(invalidId)).toThrow(
      "devices.input_board.id: must be between 0 and 31",
    );

    const invalidRole = validSource();
    invalidRole.devices.input_board = { id: 1, role: "hybrid" };
    expect(() => compileFlowDocument(invalidRole)).toThrow(
      'devices.input_board.role: has unsupported value "hybrid"',
    );

    const invalidPin = validSource();
    invalidPin.inputs.button = { device: "input_board", pin: 16, debounceMs: 25 };
    expect(() => compileFlowDocument(invalidPin)).toThrow(
      "inputs.button.pin: must be between 0 and 15",
    );

    const invalidReference = validSource();
    invalidReference.flows.primary = {
      when: [{ input: "missing", edge: "rising" }],
      actions: ["relay"],
    };
    expect(() => compileFlowDocument(invalidReference)).toThrow(
      'flows.primary.when: references unknown input "missing"',
    );

    const cycle = validSource();
    cycle.flows.primary = {
      when: [{ input: "button", edge: "rising" }],
      actions: ["relay"],
      next: "primary",
    };
    expect(() => compileFlowDocument(cycle)).toThrow("flows.primary.next: is not supported");

    const invalidOperator = validSource();
    invalidOperator.actions.relay = { device: "output_board", pin: 3, operation: "script" };
    expect(() => compileFlowDocument(invalidOperator)).toThrow(
      'actions.relay.operation: has unsupported value "script"',
    );

    const invalidTarget = validSource();
    invalidTarget.actions.relay = { device: "input_board", pin: 3, operation: "toggle" };
    expect(() => compileFlowDocument(invalidTarget)).toThrow(
      "actions.relay.device: must reference an output device",
    );
  });

  test("rejects capacity overflow and incompatible executor APIs", () => {
    const tooManyActions = validSource();
    tooManyActions.actions = Object.fromEntries(
      Array.from({ length: 17 }, (_, index) => [
        `action_${index}`,
        {
          device: "output_board",
          pin: index % 16,
          operation: "set",
          value: true,
          delayMs: index,
        },
      ]),
    );
    tooManyActions.flows.primary = {
      when: [{ input: "button", edge: "rising" }],
      actions: Object.keys(tooManyActions.actions),
    };
    expect(() => compileFlowDocument(tooManyActions)).toThrow(
      "flows.primary.actions: contains 17 entries; maximum is 16",
    );

    const newerApi = validSource();
    newerApi.executorApi = 2;
    expect(() => compileFlowDocument(newerApi, 1)).toThrow(
      "executorApi: requires API 2; target supports API 1",
    );
  });

  test("enforces the action limit across every route for one source event", () => {
    const source = validSource();
    const actionNames = Array.from({ length: 17 }, (_, index) => `action_${index}`);
    source.actions = Object.fromEntries(
      actionNames.map((name, index) => [
        name,
        {
          device: "output_board",
          pin: index % 16,
          operation: "set",
          value: true,
          delayMs: index,
        },
      ]),
    );
    source.flows = {
      alpha: {
        when: [{ input: "button", edge: "rising" }],
        actions: actionNames.slice(0, 9),
      },
      beta: {
        when: [{ input: "button", edge: "rising" }],
        actions: actionNames.slice(9),
      },
    };
    expect(() => compileFlowDocument(source)).toThrow(
      'flows.beta: source event "button:rising" emits 17 actions across matching routes; maximum is 16',
    );
  });

  test("rejects duplicate actions across changed and directional routes", () => {
    const source = validSource();
    source.flows = {
      alpha_changed: {
        when: [{ input: "button", edge: "changed" }],
        actions: ["relay"],
      },
      beta_rising: {
        when: [{ input: "button", edge: "rising" }],
        actions: ["relay"],
      },
    };
    expect(() => compileFlowDocument(source)).toThrow(
      "flows.beta_rising: action 1 would execute more than once for one rising transition",
    );
  });

  test("enforces per-device action capacity", () => {
    const source = validSource();
    const actionNames = Array.from(
      { length: 129 },
      (_, index) => `action_${index.toString().padStart(3, "0")}`,
    );
    source.inputs = Object.fromEntries(
      Array.from({ length: 9 }, (_, index) => [
        `button_${index}`,
        { device: "input_board", pin: index, debounceMs: 25 },
      ]),
    );
    source.actions = Object.fromEntries(
      actionNames.map((name, index) => [
        name,
        {
          device: "output_board",
          pin: index % 16,
          operation: "set",
          value: true,
          delayMs: index,
        },
      ]),
    );
    const eventTriggers = Array.from({ length: 9 }, (_, input) => ({
      input: `button_${input}`,
      edge: "rising",
    }));
    source.flows = Object.fromEntries(
      eventTriggers.map((trigger, index) => [
        `flow_${index}`,
        {
          when: [trigger],
          actions: actionNames.slice(index * 16, Math.min((index + 1) * 16, actionNames.length)),
        },
      ]),
    );
    expect(() => compileFlowDocument(source)).toThrow(
      "devices.output_board: owns 129 actions; maximum is 128",
    );
  });

  test("rejects a graph whose valid tables exceed the image byte limit", () => {
    const deviceEntries: [string, unknown][] = [];
    for (let id = 0; id < 16; id += 1) {
      deviceEntries.push([
        `input_device_${id.toString().padStart(2, "0")}`,
        { id, role: "input" },
      ]);
    }
    for (let id = 16; id < 32; id += 1) {
      deviceEntries.push([
        `output_device_${id.toString().padStart(2, "0")}`,
        { id, role: "output" },
      ]);
    }
    const inputEntries = Array.from({ length: 256 }, (_, index) => [
      `input_${index.toString().padStart(3, "0")}`,
      {
        device: `input_device_${Math.floor(index / 16).toString().padStart(2, "0")}`,
        pin: index % 16,
        debounceMs: 20,
      },
    ] as const);
    const actionEntries = Array.from({ length: 512 }, (_, index) => [
      `action_${index.toString().padStart(3, "0")}`,
      {
        device: `output_device_${(16 + Math.floor(index / 32)).toString().padStart(2, "0")}`,
        pin: index % 16,
        operation: "set",
        value: true,
        delayMs: index,
      },
    ] as const);
    const triggers = inputEntries.flatMap(([name]) => [
      { input: name, edge: "rising" },
      { input: name, edge: "falling" },
    ]);
    const actionNames = actionEntries.map(([name]) => name);
    const flows = Object.fromEntries(
      Array.from({ length: 32 }, (_, index) => [
        `flow_${index.toString().padStart(2, "0")}`,
        {
          when: triggers.slice(index * 16, (index + 1) * 16),
          actions: actionNames.slice(index * 16, (index + 1) * 16),
        },
      ]),
    );
    const source: MutableSource = {
      format: 1,
      executorApi: 1,
      generation: 10,
      devices: Object.fromEntries(deviceEntries),
      inputs: Object.fromEntries(inputEntries),
      actions: Object.fromEntries(actionEntries),
      flows,
    };
    expect(() => compileFlowDocument(source)).toThrow(
      "compiled image is 33336 bytes; maximum is 16384",
    );
  });
});
