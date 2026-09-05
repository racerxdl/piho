import { FlowValidationError } from "./errors.ts";
import {
  EXECUTOR_API_VERSION,
  GRAPH_FORMAT_VERSION,
  GRAPH_LIMITS,
  type ActionDefinition,
  type ActionOperation,
  type DeviceDefinition,
  type DeviceRole,
  type Edge,
  type FlowDefinition,
  type FlowTriggerDefinition,
  type InputDefinition,
  type ValidatedFlowDocument,
} from "./types.ts";

type JsonObject = Record<string, unknown>;

const NAME_PATTERN = /^[A-Za-z][A-Za-z0-9_-]{0,63}$/;
const DEVICE_ROLES: Record<DeviceRole, true> = {
  input: true,
  output: true,
};
const EDGES: Record<Edge, true> = {
  rising: true,
  falling: true,
  changed: true,
};
const OPERATIONS: Record<ActionOperation, true> = {
  set: true,
  copy_source: true,
  toggle: true,
  pulse: true,
};

function fail(path: string, message: string): never {
  throw new FlowValidationError(path, message);
}

function objectValue(value: unknown, path: string): JsonObject {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    fail(path, "must be an object");
  }
  return value as JsonObject;
}

function own(object: JsonObject, key: string): boolean {
  return Object.prototype.hasOwnProperty.call(object, key);
}

function required(object: JsonObject, key: string, path: string): unknown {
  if (!own(object, key)) {
    fail(`${path}.${key}`, "is required");
  }
  return object[key];
}

function rejectUnknown(object: JsonObject, keys: readonly string[], path: string): void {
  const allowed = new Set(keys);
  for (const key of Object.keys(object)) {
    if (!allowed.has(key)) {
      fail(`${path}.${key}`, "is not supported");
    }
  }
}

function integer(value: unknown, path: string, minimum: number, maximum: number): number {
  if (typeof value !== "number" || !Number.isSafeInteger(value)) {
    fail(path, "must be an integer");
  }
  if (value < minimum || value > maximum) {
    fail(path, `must be between ${minimum} and ${maximum}`);
  }
  return value;
}

function stringValue(value: unknown, path: string): string {
  if (typeof value !== "string") {
    fail(path, "must be a string");
  }
  return value;
}

function booleanValue(value: unknown, path: string): boolean {
  if (typeof value !== "boolean") {
    fail(path, "must be a boolean");
  }
  return value;
}

function enumValue<T extends string>(
  value: unknown,
  path: string,
  values: Readonly<Record<T, true>>,
): T {
  const text = stringValue(value, path);
  if (!Object.prototype.hasOwnProperty.call(values, text)) {
    fail(path, `has unsupported value ${JSON.stringify(text)}`);
  }
  return text as T;
}

function namedObjects(value: unknown, path: string, maximum: number): readonly [string, JsonObject][] {
  const object = objectValue(value, path);
  const names = Object.keys(object).sort((left, right) => left < right ? -1 : left > right ? 1 : 0);
  if (names.length > maximum) {
    fail(path, `contains ${names.length} entries; maximum is ${maximum}`);
  }
  return names.map((name) => {
    if (!NAME_PATTERN.test(name)) {
      fail(`${path}.${name}`, "name must match /^[A-Za-z][A-Za-z0-9_-]{0,63}$/");
    }
    return [name, objectValue(object[name], `${path}.${name}`)] as const;
  });
}

function parseDevices(value: unknown): readonly DeviceDefinition[] {
  return namedObjects(value, "devices", GRAPH_LIMITS.devices).map(([name, object]) => {
    const path = `devices.${name}`;
    rejectUnknown(object, ["id", "role"], path);
    return {
      name,
      id: integer(required(object, "id", path), `${path}.id`, 0, GRAPH_LIMITS.devices - 1),
      role: enumValue(required(object, "role", path), `${path}.role`, DEVICE_ROLES),
    };
  });
}

function parseInputs(value: unknown): readonly InputDefinition[] {
  return namedObjects(value, "inputs", GRAPH_LIMITS.inputs).map(([name, object]) => {
    const path = `inputs.${name}`;
    rejectUnknown(object, ["device", "pin", "debounceMs"], path);
    return {
      name,
      device: stringValue(required(object, "device", path), `${path}.device`),
      pin: integer(required(object, "pin", path), `${path}.pin`, 0, 15),
      debounceMs: integer(
        required(object, "debounceMs", path),
        `${path}.debounceMs`,
        0,
        GRAPH_LIMITS.debounceMs,
      ),
    };
  });
}

function parseActions(value: unknown): readonly ActionDefinition[] {
  return namedObjects(value, "actions", GRAPH_LIMITS.actions).map(([name, object]) => {
    const path = `actions.${name}`;
    const operation = enumValue(
      required(object, "operation", path),
      `${path}.operation`,
      OPERATIONS,
    );
    const keys = ["device", "pin", "operation", "delayMs"];
    if (operation === "set") {
      keys.push("value");
    } else if (operation === "pulse") {
      keys.push("durationMs");
    }
    rejectUnknown(object, keys, path);

    const delayMs = own(object, "delayMs")
      ? integer(object.delayMs, `${path}.delayMs`, 0, GRAPH_LIMITS.actionTimeMs)
      : 0;
    const value = operation === "set"
      ? booleanValue(required(object, "value", path), `${path}.value`)
      : false;
    const durationMs = operation === "pulse"
      ? integer(
          required(object, "durationMs", path),
          `${path}.durationMs`,
          1,
          GRAPH_LIMITS.actionTimeMs,
        )
      : 0;

    return {
      name,
      device: stringValue(required(object, "device", path), `${path}.device`),
      pin: integer(required(object, "pin", path), `${path}.pin`, 0, 15),
      operation,
      value,
      delayMs,
      durationMs,
    };
  });
}

function parseTrigger(value: unknown, path: string): FlowTriggerDefinition {
  const object = objectValue(value, path);
  rejectUnknown(object, ["input", "edge"], path);
  return {
    input: stringValue(required(object, "input", path), `${path}.input`),
    edge: enumValue(required(object, "edge", path), `${path}.edge`, EDGES),
  };
}

function parseFlows(value: unknown): readonly FlowDefinition[] {
  return namedObjects(value, "flows", GRAPH_LIMITS.flows).map(([name, object]) => {
    const path = `flows.${name}`;
    rejectUnknown(object, ["when", "actions"], path);

    const whenValue = required(object, "when", path);
    if (!Array.isArray(whenValue) || whenValue.length === 0) {
      fail(`${path}.when`, "must be a non-empty array");
    }
    if (whenValue.length > GRAPH_LIMITS.routes) {
      fail(
        `${path}.when`,
        `contains ${whenValue.length} entries; maximum is ${GRAPH_LIMITS.routes}`,
      );
    }
    const when = whenValue.map((trigger, index) => parseTrigger(trigger, `${path}.when[${index}]`));

    const actionsValue = required(object, "actions", path);
    if (!Array.isArray(actionsValue) || actionsValue.length === 0) {
      fail(`${path}.actions`, "must be a non-empty array");
    }
    if (actionsValue.length > GRAPH_LIMITS.actionsPerEvent) {
      fail(
        `${path}.actions`,
        `contains ${actionsValue.length} entries; maximum is ${GRAPH_LIMITS.actionsPerEvent}`,
      );
    }
    const actions = actionsValue.map((action, index) =>
      stringValue(action, `${path}.actions[${index}]`),
    );
    if (new Set(actions).size !== actions.length) {
      fail(`${path}.actions`, "contains duplicate action references");
    }

    return { name, when, actions };
  });
}

export function validateFlowDocument(
  value: unknown,
  targetExecutorApi = EXECUTOR_API_VERSION,
): ValidatedFlowDocument {
  if (
    !Number.isSafeInteger(targetExecutorApi) ||
    targetExecutorApi < 1 ||
    targetExecutorApi > 0xffff
  ) {
    fail("targetExecutorApi", "must be a 16-bit positive integer");
  }
  const root = objectValue(value, "$");
  rejectUnknown(root, ["format", "executorApi", "generation", "devices", "inputs", "actions", "flows"], "$");

  const format = integer(required(root, "format", "$"), "format", 1, 0xffff);
  if (format !== GRAPH_FORMAT_VERSION) {
    fail("format", `unsupported format ${format}; expected ${GRAPH_FORMAT_VERSION}`);
  }
  const executorApi = integer(required(root, "executorApi", "$"), "executorApi", 1, 0xffff);
  if (executorApi > targetExecutorApi) {
    fail("executorApi", `requires API ${executorApi}; target supports API ${targetExecutorApi}`);
  }
  if (executorApi !== EXECUTOR_API_VERSION) {
    fail(
      "executorApi",
      `unsupported executor API ${executorApi}; compiler supports ${EXECUTOR_API_VERSION}`,
    );
  }

  return {
    format,
    executorApi,
    generation: integer(required(root, "generation", "$"), "generation", 1, 0xffff_ffff),
    devices: parseDevices(required(root, "devices", "$")),
    inputs: parseInputs(required(root, "inputs", "$")),
    actions: parseActions(required(root, "actions", "$")),
    flows: parseFlows(required(root, "flows", "$")),
  };
}
