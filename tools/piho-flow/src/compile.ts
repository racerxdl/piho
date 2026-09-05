import { FlowValidationError } from "./errors.ts";
import { encodeGraphImage } from "./image.ts";
import {
  EXECUTOR_API_VERSION,
  GRAPH_LIMITS,
  type CompiledAction,
  type CompiledDevice,
  type CompiledGraph,
  type CompiledInput,
  type CompiledRoute,
  type Edge,
  type ValidatedFlowDocument,
} from "./types.ts";
import { validateFlowDocument } from "./validate.ts";

const EDGE_ORDER: Record<Edge, number> = {
  rising: 1,
  falling: 2,
  changed: 3,
};

function compareText(left: string, right: string): number {
  return left < right ? -1 : left > right ? 1 : 0;
}

function fail(path: string, message: string): never {
  throw new FlowValidationError(path, message);
}

function countForDevice<T>(items: readonly T[], device: number, select: (item: T) => number): number {
  let count = 0;
  for (const item of items) {
    if (select(item) === device) {
      count += 1;
    }
  }
  return count;
}

function startForDevice<T>(items: readonly T[], device: number, select: (item: T) => number): number {
  const index = items.findIndex((item) => select(item) === device);
  return index < 0 ? 0 : index;
}

export function compileGraphModel(document: ValidatedFlowDocument): CompiledGraph {
  if (document.devices.length === 0) {
    fail("devices", "must contain at least one board");
  }

  const devicesByName = new Map(document.devices.map((device) => [device.name, device]));
  const deviceNamesById = new Map<number, string>();
  for (const device of document.devices) {
    const duplicate = deviceNamesById.get(device.id);
    if (duplicate !== undefined) {
      fail(`devices.${device.name}.id`, `duplicates device ID used by ${JSON.stringify(duplicate)}`);
    }
    deviceNamesById.set(device.id, device.name);
  }

  const inputIds = new Map(
    [...document.inputs]
      .sort((left, right) => compareText(left.name, right.name))
      .map((input, index) => [input.name, index + 1]),
  );
  const inputNamesByEndpoint = new Map<string, string>();
  const compiledInputsByName = new Map<string, CompiledInput>();
  for (const input of document.inputs) {
    const device = devicesByName.get(input.device);
    if (device === undefined) {
      fail(`inputs.${input.name}.device`, `references unknown device ${JSON.stringify(input.device)}`);
    }
    if (device.role !== "input") {
      fail(`inputs.${input.name}.device`, "must reference an input device");
    }
    const endpoint = `${device.id}:${input.pin}`;
    const duplicate = inputNamesByEndpoint.get(endpoint);
    if (duplicate !== undefined) {
      fail(`inputs.${input.name}`, `duplicates the physical endpoint used by ${JSON.stringify(duplicate)}`);
    }
    inputNamesByEndpoint.set(endpoint, input.name);
    const id = inputIds.get(input.name);
    if (id === undefined) {
      throw new Error("internal input ID assignment failure");
    }
    compiledInputsByName.set(input.name, {
      id,
      device: device.id,
      pin: input.pin,
      debounceMs: input.debounceMs,
    });
  }

  const actionIds = new Map(
    [...document.actions]
      .sort((left, right) => compareText(left.name, right.name))
      .map((action, index) => [action.name, index + 1]),
  );
  const actionNamesByDefinition = new Map<string, string>();
  const compiledActionsByName = new Map<string, CompiledAction>();
  for (const action of document.actions) {
    const device = devicesByName.get(action.device);
    if (device === undefined) {
      fail(`actions.${action.name}.device`, `references unknown device ${JSON.stringify(action.device)}`);
    }
    if (device.role !== "output") {
      fail(`actions.${action.name}.device`, "must reference an output device");
    }
    const definition = [
      device.id,
      action.pin,
      action.operation,
      action.value ? 1 : 0,
      action.delayMs,
      action.durationMs,
    ].join(":");
    const duplicate = actionNamesByDefinition.get(definition);
    if (duplicate !== undefined) {
      fail(`actions.${action.name}`, `duplicates action ${JSON.stringify(duplicate)}; reuse that action instead`);
    }
    actionNamesByDefinition.set(definition, action.name);
    const id = actionIds.get(action.name);
    if (id === undefined) {
      throw new Error("internal action ID assignment failure");
    }
    compiledActionsByName.set(action.name, {
      id,
      targetDevice: device.id,
      targetPin: action.pin,
      operation: action.operation,
      value: action.value,
      delayMs: action.delayMs,
      durationMs: action.durationMs,
    });
  }

  const flows = [...document.flows].sort((left, right) => compareText(left.name, right.name));
  const usedInputs = new Set<string>();
  const usedActions = new Set<string>();
  const routeActionKeys = new Set<string>();
  const actionCountByEvent = new Map<string, number>();
  const routes: CompiledRoute[] = [];
  for (let flowIndex = 0; flowIndex < flows.length; flowIndex += 1) {
    const flow = flows[flowIndex];
    if (flow === undefined) {
      throw new Error("internal flow indexing failure");
    }
    const flowId = flowIndex + 1;
    const resolvedActions = flow.actions.map((name) => {
      const action = compiledActionsByName.get(name);
      if (action === undefined) {
        fail(`flows.${flow.name}.actions`, `references unknown action ${JSON.stringify(name)}`);
      }
      usedActions.add(name);
      return action;
    }).sort((left, right) => left.id - right.id);

    const triggers = [...flow.when].sort((left, right) => {
      const inputOrder = compareText(left.input, right.input);
      return inputOrder !== 0 ? inputOrder : EDGE_ORDER[left.edge] - EDGE_ORDER[right.edge];
    });
    const triggerKeys = new Set<string>();
    for (const trigger of triggers) {
      const triggerKey = `${trigger.input}:${trigger.edge}`;
      if (triggerKeys.has(triggerKey)) {
        fail(`flows.${flow.name}.when`, `contains duplicate trigger ${JSON.stringify(triggerKey)}`);
      }
      triggerKeys.add(triggerKey);

      const input = compiledInputsByName.get(trigger.input);
      if (input === undefined) {
        fail(`flows.${flow.name}.when`, `references unknown input ${JSON.stringify(trigger.input)}`);
      }
      usedInputs.add(trigger.input);
      const eventKey = `${input.id}:${trigger.edge}`;
      const eventActionCount = (actionCountByEvent.get(eventKey) ?? 0) + resolvedActions.length;
      if (eventActionCount > GRAPH_LIMITS.actionsPerEvent) {
        fail(
          `flows.${flow.name}`,
          `source event ${JSON.stringify(triggerKey)} emits ${eventActionCount} actions across flows; maximum is ${GRAPH_LIMITS.actionsPerEvent}`,
        );
      }
      actionCountByEvent.set(eventKey, eventActionCount);
      for (const action of resolvedActions) {
        if (action.operation === "copy_source" && trigger.edge !== "changed") {
          fail(
            `flows.${flow.name}`,
            `copy_source action ${action.id} requires every trigger edge to be changed`,
          );
        }
        const routeActionKey = `${input.id}:${trigger.edge}:${action.id}`;
        if (routeActionKeys.has(routeActionKey)) {
          fail(
            `flows.${flow.name}`,
            "duplicates an input, edge, and action combination from another flow",
          );
        }
        routeActionKeys.add(routeActionKey);
      }

      routes.push({
        id: routes.length + 1,
        flowId,
        inputId: input.id,
        ownerDevice: input.device,
        edge: trigger.edge,
        actionIds: resolvedActions.map((action) => action.id),
      });
    }
  }

  if (routes.length > GRAPH_LIMITS.routes) {
    fail("flows", `compile to ${routes.length} routes; maximum is ${GRAPH_LIMITS.routes}`);
  }
  for (const input of document.inputs) {
    if (!usedInputs.has(input.name)) {
      fail(`inputs.${input.name}`, "is not referenced by any flow");
    }
  }
  for (const action of document.actions) {
    if (!usedActions.has(action.name)) {
      fail(`actions.${action.name}`, "is not referenced by any flow");
    }
  }

  const inputs = [...compiledInputsByName.values()].sort(
    (left, right) => left.device - right.device || left.id - right.id,
  );
  routes.sort((left, right) => left.ownerDevice - right.ownerDevice || left.id - right.id);
  const actions = [...compiledActionsByName.values()].sort(
    (left, right) => left.targetDevice - right.targetDevice || left.id - right.id,
  );

  const devices: CompiledDevice[] = [...document.devices]
    .sort((left, right) => left.id - right.id)
    .map((device) => {
      const inputCount = countForDevice(inputs, device.id, (input) => input.device);
      const routeCount = countForDevice(routes, device.id, (route) => route.ownerDevice);
      const actionCount = countForDevice(actions, device.id, (action) => action.targetDevice);
      if (inputCount > 16) {
        fail(`devices.${device.name}`, `has ${inputCount} inputs; maximum is 16`);
      }
      if (routeCount > GRAPH_LIMITS.localRoutes) {
        fail(
          `devices.${device.name}`,
          `owns ${routeCount} routes; maximum is ${GRAPH_LIMITS.localRoutes}`,
        );
      }
      if (actionCount > GRAPH_LIMITS.localActions) {
        fail(
          `devices.${device.name}`,
          `owns ${actionCount} actions; maximum is ${GRAPH_LIMITS.localActions}`,
        );
      }
      return {
        id: device.id,
        role: device.role,
        inputStart: startForDevice(inputs, device.id, (input) => input.device),
        inputCount,
        routeStart: startForDevice(routes, device.id, (route) => route.ownerDevice),
        routeCount,
        actionStart: startForDevice(actions, device.id, (action) => action.targetDevice),
        actionCount,
      };
    });

  return {
    format: document.format,
    executorApi: document.executorApi,
    generation: document.generation,
    flowCount: flows.length,
    devices,
    inputs,
    routes,
    actions,
  };
}

export interface CompilationResult {
  readonly graph: CompiledGraph;
  readonly image: Uint8Array;
}

export function compileFlowDocument(
  value: unknown,
  targetExecutorApi = EXECUTOR_API_VERSION,
): CompilationResult {
  const graph = compileGraphModel(validateFlowDocument(value, targetExecutorApi));
  return { graph, image: encodeGraphImage(graph) };
}
