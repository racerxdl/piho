import { GraphImageError } from "./errors.ts";
import {
  EXECUTOR_API_VERSION,
  GRAPH_FORMAT_VERSION,
  GRAPH_LIMITS,
  type ActionOperation,
  type CompiledAction,
  type CompiledDevice,
  type CompiledGraph,
  type CompiledInput,
  type CompiledRoute,
  type DeviceRole,
  type Edge,
} from "./types.ts";

export const GRAPH_HEADER_SIZE = 56;
export const GRAPH_DEVICE_RECORD_SIZE = 16;
export const GRAPH_INPUT_RECORD_SIZE = 8;
export const GRAPH_ROUTE_RECORD_SIZE = 12;
export const GRAPH_ACTION_REFERENCE_SIZE = 2;
export const GRAPH_ACTION_RECORD_SIZE = 16;
export const GRAPH_CHECKSUM_OFFSET = 52;

const MAGIC = [0x50, 0x48, 0x47, 0x46] as const;

const ROLE_CODE: Record<DeviceRole, number> = {
  input: 1,
  output: 2,
};

const EDGE_CODE: Record<Edge, number> = {
  rising: 1,
  falling: 2,
  changed: 3,
};

const OPERATION_CODE: Record<ActionOperation, number> = {
  set: 1,
  copy_source: 2,
  toggle: 3,
  pulse: 4,
};

function imageError(message: string): never {
  throw new GraphImageError(message);
}

function decodeRole(code: number): DeviceRole {
  switch (code) {
    case 1:
      return "input";
    case 2:
      return "output";
    default:
      return imageError(`invalid device role code ${code}`);
  }
}

function decodeEdge(code: number): Edge {
  switch (code) {
    case 1:
      return "rising";
    case 2:
      return "falling";
    case 3:
      return "changed";
    default:
      return imageError(`invalid edge code ${code}`);
  }
}

function decodeOperation(code: number): ActionOperation {
  switch (code) {
    case 1:
      return "set";
    case 2:
      return "copy_source";
    case 3:
      return "toggle";
    case 4:
      return "pulse";
    default:
      return imageError(`invalid action operation code ${code}`);
  }
}

export function graphImageCrc32(bytes: Uint8Array): number {
  let crc = 0xffff_ffff;
  for (let index = 0; index < bytes.length; index += 1) {
    const byte = index >= GRAPH_CHECKSUM_OFFSET && index < GRAPH_CHECKSUM_OFFSET + 4
      ? 0
      : bytes[index];
    if (byte === undefined) {
      throw new Error("internal CRC indexing failure");
    }
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 1) !== 0 ? (crc >>> 1) ^ 0xedb8_8320 : crc >>> 1;
    }
  }
  return (~crc) >>> 0;
}

function actionReferenceCount(graph: CompiledGraph): number {
  let count = 0;
  for (const route of graph.routes) {
    count += route.actionIds.length;
  }
  return count;
}

export function encodeGraphImage(graph: CompiledGraph): Uint8Array {
  const referenceCount = actionReferenceCount(graph);
  const deviceOffset = GRAPH_HEADER_SIZE;
  const inputOffset = deviceOffset + graph.devices.length * GRAPH_DEVICE_RECORD_SIZE;
  const routeOffset = inputOffset + graph.inputs.length * GRAPH_INPUT_RECORD_SIZE;
  const actionReferenceOffset = routeOffset + graph.routes.length * GRAPH_ROUTE_RECORD_SIZE;
  const actionOffset = actionReferenceOffset + referenceCount * GRAPH_ACTION_REFERENCE_SIZE;
  const imageLength = actionOffset + graph.actions.length * GRAPH_ACTION_RECORD_SIZE;
  if (imageLength > GRAPH_LIMITS.imageBytes) {
    imageError(`compiled image is ${imageLength} bytes; maximum is ${GRAPH_LIMITS.imageBytes}`);
  }
  for (const count of [
    graph.devices.length,
    graph.inputs.length,
    graph.flowCount,
    graph.routes.length,
    referenceCount,
    graph.actions.length,
  ]) {
    if (count > 0xffff) {
      imageError(`record count ${count} exceeds the 16-bit image format`);
    }
  }

  const bytes = new Uint8Array(imageLength);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  bytes.set(MAGIC, 0);
  view.setUint16(4, graph.format, true);
  view.setUint16(6, graph.executorApi, true);
  view.setUint32(8, graph.generation, true);
  view.setUint32(12, imageLength, true);
  view.setUint16(16, graph.devices.length, true);
  view.setUint16(18, graph.inputs.length, true);
  view.setUint16(20, graph.flowCount, true);
  view.setUint16(22, graph.routes.length, true);
  view.setUint16(24, referenceCount, true);
  view.setUint16(26, graph.actions.length, true);
  view.setUint32(28, 0, true);
  view.setUint32(32, deviceOffset, true);
  view.setUint32(36, inputOffset, true);
  view.setUint32(40, routeOffset, true);
  view.setUint32(44, actionReferenceOffset, true);
  view.setUint32(48, actionOffset, true);
  view.setUint32(GRAPH_CHECKSUM_OFFSET, 0, true);

  graph.devices.forEach((device, index) => {
    const offset = deviceOffset + index * GRAPH_DEVICE_RECORD_SIZE;
    view.setUint8(offset, device.id);
    view.setUint8(offset + 1, ROLE_CODE[device.role]);
    view.setUint16(offset + 2, device.inputStart, true);
    view.setUint16(offset + 4, device.inputCount, true);
    view.setUint16(offset + 6, device.routeStart, true);
    view.setUint16(offset + 8, device.routeCount, true);
    view.setUint16(offset + 10, device.actionStart, true);
    view.setUint16(offset + 12, device.actionCount, true);
    view.setUint16(offset + 14, 0, true);
  });

  graph.inputs.forEach((input, index) => {
    const offset = inputOffset + index * GRAPH_INPUT_RECORD_SIZE;
    view.setUint16(offset, input.id, true);
    view.setUint8(offset + 2, input.device);
    view.setUint8(offset + 3, input.pin);
    view.setUint16(offset + 4, input.debounceMs, true);
    view.setUint16(offset + 6, 0, true);
  });

  let referenceIndex = 0;
  graph.routes.forEach((route, index) => {
    const offset = routeOffset + index * GRAPH_ROUTE_RECORD_SIZE;
    view.setUint16(offset, route.id, true);
    view.setUint16(offset + 2, route.flowId, true);
    view.setUint16(offset + 4, route.inputId, true);
    view.setUint16(offset + 6, referenceIndex, true);
    view.setUint8(offset + 8, route.ownerDevice);
    view.setUint8(offset + 9, EDGE_CODE[route.edge]);
    view.setUint8(offset + 10, route.actionIds.length);
    view.setUint8(offset + 11, 0);
    for (const actionId of route.actionIds) {
      view.setUint16(
        actionReferenceOffset + referenceIndex * GRAPH_ACTION_REFERENCE_SIZE,
        actionId,
        true,
      );
      referenceIndex += 1;
    }
  });

  graph.actions.forEach((action, index) => {
    const offset = actionOffset + index * GRAPH_ACTION_RECORD_SIZE;
    view.setUint16(offset, action.id, true);
    view.setUint8(offset + 2, action.targetDevice);
    view.setUint8(offset + 3, action.targetPin);
    view.setUint8(offset + 4, OPERATION_CODE[action.operation]);
    view.setUint8(offset + 5, action.value ? 1 : 0);
    view.setUint16(offset + 6, 0, true);
    view.setUint32(offset + 8, action.delayMs, true);
    view.setUint32(offset + 12, action.durationMs, true);
  });

  view.setUint32(GRAPH_CHECKSUM_OFFSET, graphImageCrc32(bytes), true);
  return bytes;
}

function validateCanonicalOffsets(
  imageLength: number,
  deviceCount: number,
  inputCount: number,
  routeCount: number,
  referenceCount: number,
  actionCount: number,
  deviceOffset: number,
  inputOffset: number,
  routeOffset: number,
  referenceOffset: number,
  actionOffset: number,
): void {
  const expectedDeviceOffset = GRAPH_HEADER_SIZE;
  const expectedInputOffset = expectedDeviceOffset + deviceCount * GRAPH_DEVICE_RECORD_SIZE;
  const expectedRouteOffset = expectedInputOffset + inputCount * GRAPH_INPUT_RECORD_SIZE;
  const expectedReferenceOffset = expectedRouteOffset + routeCount * GRAPH_ROUTE_RECORD_SIZE;
  const expectedActionOffset = expectedReferenceOffset + referenceCount * GRAPH_ACTION_REFERENCE_SIZE;
  const expectedLength = expectedActionOffset + actionCount * GRAPH_ACTION_RECORD_SIZE;
  const actual = [deviceOffset, inputOffset, routeOffset, referenceOffset, actionOffset, imageLength];
  const expected = [
    expectedDeviceOffset,
    expectedInputOffset,
    expectedRouteOffset,
    expectedReferenceOffset,
    expectedActionOffset,
    expectedLength,
  ];
  if (actual.some((value, index) => value !== expected[index])) {
    imageError("record offsets or image length are not canonical");
  }
}

function validateDeviceSlices(
  devices: readonly CompiledDevice[],
  inputs: readonly CompiledInput[],
  routes: readonly CompiledRoute[],
  actions: readonly CompiledAction[],
): void {
  let inputStart = 0;
  let routeStart = 0;
  let actionStart = 0;
  for (const device of devices) {
    if (device.inputCount > 16) {
      imageError(`device ${device.id} exceeds the local input limit`);
    }
    if (device.routeCount > GRAPH_LIMITS.localRoutes) {
      imageError(`device ${device.id} exceeds the local route limit`);
    }
    if (device.actionCount > GRAPH_LIMITS.localActions) {
      imageError(`device ${device.id} exceeds the local action limit`);
    }

    if (device.inputCount === 0) {
      if (device.inputStart !== 0) {
        imageError(`device ${device.id} has a noncanonical empty input slice`);
      }
    } else {
      if (device.inputStart !== inputStart) {
        imageError(`device ${device.id} has a noncanonical input slice`);
      }
      const slice = inputs.slice(inputStart, inputStart + device.inputCount);
      if (slice.length !== device.inputCount || slice.some((input) => input.device !== device.id)) {
        imageError(`device ${device.id} has an invalid input slice`);
      }
      inputStart += device.inputCount;
    }

    if (device.routeCount === 0) {
      if (device.routeStart !== 0) {
        imageError(`device ${device.id} has a noncanonical empty route slice`);
      }
    } else {
      if (device.routeStart !== routeStart) {
        imageError(`device ${device.id} has a noncanonical route slice`);
      }
      const slice = routes.slice(routeStart, routeStart + device.routeCount);
      if (
        slice.length !== device.routeCount ||
        slice.some((route) => route.ownerDevice !== device.id)
      ) {
        imageError(`device ${device.id} has an invalid route slice`);
      }
      routeStart += device.routeCount;
    }

    if (device.actionCount === 0) {
      if (device.actionStart !== 0) {
        imageError(`device ${device.id} has a noncanonical empty action slice`);
      }
    } else {
      if (device.actionStart !== actionStart) {
        imageError(`device ${device.id} has a noncanonical action slice`);
      }
      const slice = actions.slice(actionStart, actionStart + device.actionCount);
      if (
        slice.length !== device.actionCount ||
        slice.some((action) => action.targetDevice !== device.id)
      ) {
        imageError(`device ${device.id} has an invalid action slice`);
      }
      actionStart += device.actionCount;
    }
  }
  if (inputStart !== inputs.length || routeStart !== routes.length || actionStart !== actions.length) {
    imageError("device slices do not cover every graph record");
  }
}

export function decodeGraphImage(bytes: Uint8Array): CompiledGraph {
  if (bytes.length < GRAPH_HEADER_SIZE || bytes.length > GRAPH_LIMITS.imageBytes) {
    imageError(`image length ${bytes.length} is outside the supported range`);
  }
  for (let index = 0; index < MAGIC.length; index += 1) {
    if (bytes[index] !== MAGIC[index]) {
      imageError("invalid graph image magic");
    }
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const format = view.getUint16(4, true);
  const executorApi = view.getUint16(6, true);
  const generation = view.getUint32(8, true);
  const imageLength = view.getUint32(12, true);
  const deviceCount = view.getUint16(16, true);
  const inputCount = view.getUint16(18, true);
  const flowCount = view.getUint16(20, true);
  const routeCount = view.getUint16(22, true);
  const referenceCount = view.getUint16(24, true);
  const actionCount = view.getUint16(26, true);
  const reserved = view.getUint32(28, true);
  const deviceOffset = view.getUint32(32, true);
  const inputOffset = view.getUint32(36, true);
  const routeOffset = view.getUint32(40, true);
  const referenceOffset = view.getUint32(44, true);
  const actionOffset = view.getUint32(48, true);
  const storedChecksum = view.getUint32(GRAPH_CHECKSUM_OFFSET, true);

  if (format !== GRAPH_FORMAT_VERSION) {
    imageError(`unsupported graph format ${format}`);
  }
  if (executorApi !== EXECUTOR_API_VERSION) {
    imageError(`unsupported executor API ${executorApi}`);
  }
  if (generation === 0) {
    imageError("graph generation must be nonzero");
  }
  if (imageLength !== bytes.length || reserved !== 0) {
    imageError("invalid graph header length or reserved field");
  }
  if (
    deviceCount === 0 ||
    deviceCount > GRAPH_LIMITS.devices ||
    inputCount > GRAPH_LIMITS.inputs ||
    flowCount > GRAPH_LIMITS.flows ||
    routeCount > GRAPH_LIMITS.routes ||
    actionCount > GRAPH_LIMITS.actions
  ) {
    imageError("graph header count exceeds a format-1 limit");
  }
  validateCanonicalOffsets(
    imageLength,
    deviceCount,
    inputCount,
    routeCount,
    referenceCount,
    actionCount,
    deviceOffset,
    inputOffset,
    routeOffset,
    referenceOffset,
    actionOffset,
  );
  if (graphImageCrc32(bytes) !== storedChecksum) {
    imageError("graph image checksum mismatch");
  }

  const devices: CompiledDevice[] = [];
  const deviceRoles = new Map<number, DeviceRole>();
  for (let index = 0; index < deviceCount; index += 1) {
    const offset = deviceOffset + index * GRAPH_DEVICE_RECORD_SIZE;
    const device: CompiledDevice = {
      id: view.getUint8(offset),
      role: decodeRole(view.getUint8(offset + 1)),
      inputStart: view.getUint16(offset + 2, true),
      inputCount: view.getUint16(offset + 4, true),
      routeStart: view.getUint16(offset + 6, true),
      routeCount: view.getUint16(offset + 8, true),
      actionStart: view.getUint16(offset + 10, true),
      actionCount: view.getUint16(offset + 12, true),
    };
    if (view.getUint16(offset + 14, true) !== 0 || device.id >= GRAPH_LIMITS.devices) {
      imageError(`invalid device record ${index}`);
    }
    const previousDevice = devices.at(-1);
    if (previousDevice !== undefined && previousDevice.id >= device.id) {
      imageError("device records are not strictly ordered by ID");
    }
    if (deviceRoles.has(device.id)) {
      imageError(`duplicate device ID ${device.id}`);
    }
    deviceRoles.set(device.id, device.role);
    devices.push(device);
  }

  const inputs: CompiledInput[] = [];
  const inputsById = new Map<number, CompiledInput>();
  const inputEndpoints = new Set<string>();
  for (let index = 0; index < inputCount; index += 1) {
    const offset = inputOffset + index * GRAPH_INPUT_RECORD_SIZE;
    const input: CompiledInput = {
      id: view.getUint16(offset, true),
      device: view.getUint8(offset + 2),
      pin: view.getUint8(offset + 3),
      debounceMs: view.getUint16(offset + 4, true),
    };
    const endpoint = `${input.device}:${input.pin}`;
    const previousInput = inputs.at(-1);
    if (
      input.id === 0 ||
      inputsById.has(input.id) ||
      inputEndpoints.has(endpoint) ||
      deviceRoles.get(input.device) !== "input" ||
      input.pin >= 16 ||
      input.debounceMs > GRAPH_LIMITS.debounceMs ||
      (previousInput !== undefined &&
        (previousInput.device > input.device ||
          (previousInput.device === input.device && previousInput.id >= input.id))) ||
      view.getUint16(offset + 6, true) !== 0
    ) {
      imageError(`invalid input record ${index}`);
    }
    inputsById.set(input.id, input);
    inputEndpoints.add(endpoint);
    inputs.push(input);
  }
  for (let id = 1; id <= inputCount; id += 1) {
    if (!inputsById.has(id)) {
      imageError("input IDs are not contiguous");
    }
  }

  const actions: CompiledAction[] = [];
  const actionsById = new Map<number, CompiledAction>();
  const actionDefinitions = new Set<string>();
  for (let index = 0; index < actionCount; index += 1) {
    const offset = actionOffset + index * GRAPH_ACTION_RECORD_SIZE;
    const operation = decodeOperation(view.getUint8(offset + 4));
    const valueByte = view.getUint8(offset + 5);
    const action: CompiledAction = {
      id: view.getUint16(offset, true),
      targetDevice: view.getUint8(offset + 2),
      targetPin: view.getUint8(offset + 3),
      operation,
      value: valueByte !== 0,
      delayMs: view.getUint32(offset + 8, true),
      durationMs: view.getUint32(offset + 12, true),
    };
    const definition = [
      action.targetDevice,
      action.targetPin,
      operation,
      valueByte,
      action.delayMs,
      action.durationMs,
    ].join(":");
    const previousAction = actions.at(-1);
    const invalidParameters =
      (operation === "set" && valueByte > 1) ||
      (operation !== "set" && valueByte !== 0) ||
      (operation === "pulse" &&
        (action.durationMs === 0 || action.durationMs > GRAPH_LIMITS.actionTimeMs)) ||
      (operation !== "pulse" && action.durationMs !== 0);
    if (
      action.id === 0 ||
      actionsById.has(action.id) ||
      actionDefinitions.has(definition) ||
      deviceRoles.get(action.targetDevice) !== "output" ||
      action.targetPin >= 16 ||
      action.delayMs > GRAPH_LIMITS.actionTimeMs ||
      invalidParameters ||
      (previousAction !== undefined &&
        (previousAction.targetDevice > action.targetDevice ||
          (previousAction.targetDevice === action.targetDevice &&
            previousAction.id >= action.id))) ||
      view.getUint16(offset + 6, true) !== 0
    ) {
      imageError(`invalid action record ${index}`);
    }
    actionsById.set(action.id, action);
    actionDefinitions.add(definition);
    actions.push(action);
  }
  for (let id = 1; id <= actionCount; id += 1) {
    if (!actionsById.has(id)) {
      imageError("action IDs are not contiguous");
    }
  }

  const routes: CompiledRoute[] = [];
  const routeIds = new Set<number>();
  const observedFlowIds = new Set<number>();
  const usedInputIds = new Set<number>();
  const usedActionIds = new Set<number>();
  const flowTriggerKeys = new Set<string>();
  const routeActionKeys = new Set<string>();
  const actionCountByEvent = new Map<string, number>();
  let expectedReferenceStart = 0;
  for (let index = 0; index < routeCount; index += 1) {
    const offset = routeOffset + index * GRAPH_ROUTE_RECORD_SIZE;
    const id = view.getUint16(offset, true);
    const flowId = view.getUint16(offset + 2, true);
    const inputId = view.getUint16(offset + 4, true);
    const firstReference = view.getUint16(offset + 6, true);
    const ownerDevice = view.getUint8(offset + 8);
    const edge = decodeEdge(view.getUint8(offset + 9));
    const routeActionCount = view.getUint8(offset + 10);
    const previousRoute = routes.at(-1);
    const routeOrder = previousRoute === undefined
      ? -1
      : previousRoute.ownerDevice - ownerDevice ||
        previousRoute.inputId - inputId ||
        EDGE_CODE[previousRoute.edge] - EDGE_CODE[edge] ||
        previousRoute.flowId - flowId ||
        previousRoute.id - id;
    if (
      id === 0 ||
      routeIds.has(id) ||
      flowId === 0 ||
      flowId > flowCount ||
      !inputsById.has(inputId) ||
      deviceRoles.get(ownerDevice) !== "input" ||
      routeActionCount === 0 ||
      routeActionCount > GRAPH_LIMITS.actionsPerEvent ||
      firstReference !== expectedReferenceStart ||
      firstReference + routeActionCount > referenceCount ||
      routeOrder >= 0 ||
      view.getUint8(offset + 11) !== 0
    ) {
      imageError(`invalid route record ${index}`);
    }
    const input = inputsById.get(inputId);
    if (input?.device !== ownerDevice) {
      imageError(`route ${id} is not owned by its source input device`);
    }
    const flowTriggerKey = `${flowId}:${inputId}:${edge}`;
    if (flowTriggerKeys.has(flowTriggerKey)) {
      imageError(`route ${id} duplicates a trigger within one flow`);
    }
    flowTriggerKeys.add(flowTriggerKey);

    const eventKey = `${inputId}:${edge}`;
    const eventActionCount = (actionCountByEvent.get(eventKey) ?? 0) + routeActionCount;
    if (eventActionCount > GRAPH_LIMITS.actionsPerEvent) {
      imageError(`source event ${eventKey} exceeds the action emission limit`);
    }
    actionCountByEvent.set(eventKey, eventActionCount);

    const routeActionIds: number[] = [];
    for (let reference = 0; reference < routeActionCount; reference += 1) {
      const actionId = view.getUint16(
        referenceOffset + (firstReference + reference) * GRAPH_ACTION_REFERENCE_SIZE,
        true,
      );
      const action = actionsById.get(actionId);
      const previousActionId = routeActionIds.at(-1);
      const routeActionKey = `${eventKey}:${actionId}`;
      if (
        action === undefined ||
        routeActionKeys.has(routeActionKey) ||
        (previousActionId !== undefined && previousActionId >= actionId)
      ) {
        imageError(`route ${id} contains a noncanonical action reference`);
      }
      if (action.operation === "copy_source" && edge !== "changed") {
        imageError(`route ${id} uses copy_source without a changed edge`);
      }
      routeActionKeys.add(routeActionKey);
      usedActionIds.add(actionId);
      routeActionIds.push(actionId);
    }
    expectedReferenceStart += routeActionCount;
    routeIds.add(id);
    observedFlowIds.add(flowId);
    usedInputIds.add(inputId);
    routes.push({ id, flowId, inputId, ownerDevice, edge, actionIds: routeActionIds });
  }
  for (let id = 1; id <= routeCount; id += 1) {
    if (!routeIds.has(id)) {
      imageError("route IDs are not contiguous");
    }
  }
  if (
    expectedReferenceStart !== referenceCount ||
    observedFlowIds.size !== flowCount ||
    usedInputIds.size !== inputCount ||
    usedActionIds.size !== actionCount
  ) {
    imageError("graph contains inconsistent counts or unused records");
  }

  validateDeviceSlices(devices, inputs, routes, actions);
  return { format, executorApi, generation, flowCount, devices, inputs, routes, actions };
}
