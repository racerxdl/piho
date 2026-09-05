import { decodeGraphImage, GRAPH_CHECKSUM_OFFSET } from "./image.ts";
import {
  AckOperation,
  GRAPH_CHUNK_BYTES,
  GraphNodeStatusPart,
  GraphStoreError,
  GraphUpdateError,
  GraphUpdateState,
  type DeviceEvent,
  type DeviceRole,
  type GraphNodeStatusDeviceEvent,
  type GraphUpdateDeviceEvent,
  type HostGraphCommand,
} from "./protocol.ts";
import { PihoSerialClient } from "./serial.ts";
import type { CompiledGraph } from "./types.ts";

const ALL_NODE_STATUS_PARTS = 0x1ff;
const DEFAULT_OPERATION_TIMEOUT_MS = 5_000;
const DISCOVERY_QUIET_MS = 250;
const COMMAND_ATTEMPTS = 3;

export interface GraphIdentityReport {
  readonly generation: number;
  readonly checksum: number;
}

export interface NetworkNodeReport {
  readonly id: number;
  readonly complete: boolean;
  readonly role: DeviceRole | null;
  readonly format: number | null;
  readonly executorApi: number | null;
  readonly transferId: number;
  readonly updateState: GraphUpdateState;
  readonly updateError: GraphUpdateError;
  readonly storeState: number;
  readonly storeError: GraphStoreError;
  readonly active: GraphIdentityReport | null;
  readonly staged: GraphIdentityReport | null;
  readonly rollback: GraphIdentityReport | null;
  readonly activeDevices: number;
  readonly stagedDevices: number;
  readonly rollbackDevices: number;
  readonly rxDropped: number;
  readonly txDropped: number;
  readonly busErrors: number;
}

export interface NetworkIssue {
  readonly kind:
    | "missing"
    | "unexpected"
    | "incomplete_status"
    | "wrong_role"
    | "incompatible_format"
    | "incompatible_executor"
    | "storage"
    | "active_mismatch"
    | "staged_mismatch"
    | "rollback_mismatch"
    | "rejected"
    | "timeout"
    | "transport";
  readonly devices: readonly number[];
  readonly detail: string;
  readonly severity: "error" | "warning";
}

export interface NetworkStatusReport {
  readonly operation: "status";
  readonly ok: boolean;
  readonly expectedDevices: readonly number[];
  readonly discoveredDevices: readonly number[];
  readonly missingDevices: readonly number[];
  readonly activeAgreement: GraphIdentityReport | null;
  readonly nodes: readonly NetworkNodeReport[];
  readonly issues: readonly NetworkIssue[];
}

export interface DeploymentProgress {
  readonly phase: "preflight" | "transfer" | "staged" | "activation" | "complete";
  readonly completedChunks: number;
  readonly totalChunks: number;
}

export interface DeploymentOptions {
  readonly timeoutMs?: number;
  readonly onProgress?: (progress: DeploymentProgress) => void;
}

export interface DeploymentReport {
  readonly operation: "deploy";
  readonly ok: boolean;
  readonly phase: DeploymentProgress["phase"];
  readonly activated: boolean;
  readonly identity: GraphIdentityReport;
  readonly expectedDevices: readonly number[];
  readonly missingDevices: readonly number[];
  readonly rejectedDevices: readonly number[];
  readonly completedChunks: number;
  readonly totalChunks: number;
  readonly nodes: readonly NetworkNodeReport[];
  readonly issues: readonly NetworkIssue[];
}

export interface RollbackReport {
  readonly operation: "rollback";
  readonly ok: boolean;
  readonly identity: GraphIdentityReport | null;
  readonly expectedDevices: readonly number[];
  readonly missingDevices: readonly number[];
  readonly rejectedDevices: readonly number[];
  readonly nodes: readonly NetworkNodeReport[];
  readonly issues: readonly NetworkIssue[];
}

interface MutableNodeStatus {
  id: number;
  parts: number;
  role: DeviceRole | null;
  format: number | null;
  executorApi: number | null;
  transferId: number;
  updateState: GraphUpdateState;
  updateError: GraphUpdateError;
  storeState: number;
  storeError: GraphStoreError;
  active: GraphIdentityReport | null;
  staged: GraphIdentityReport | null;
  rollback: GraphIdentityReport | null;
  activeDevices: number;
  stagedDevices: number;
  rollbackDevices: number;
  rxDropped: number;
  txDropped: number;
  busErrors: number;
}

function emptyNode(id: number): MutableNodeStatus {
  return {
    id,
    parts: 0,
    role: null,
    format: null,
    executorApi: null,
    transferId: 0,
    updateState: GraphUpdateState.Idle,
    updateError: GraphUpdateError.None,
    storeState: 0,
    storeError: GraphStoreError.None,
    active: null,
    staged: null,
    rollback: null,
    activeDevices: 0,
    stagedDevices: 0,
    rollbackDevices: 0,
    rxDropped: 0,
    txDropped: 0,
    busErrors: 0,
  };
}

function identity(generation: number, checksum: number): GraphIdentityReport | null {
  return generation === 0 ? null : { generation, checksum };
}

function ingestNodeStatus(nodes: Map<number, MutableNodeStatus>, event: GraphNodeStatusDeviceEvent): void {
  const node = nodes.get(event.device) ?? emptyNode(event.device);
  if (event.part >= GraphNodeStatusPart.Capabilities &&
      event.part <= GraphNodeStatusPart.TransportErrors) {
    node.parts |= 1 << (event.part - 1);
  }
  switch (event.part) {
    case GraphNodeStatusPart.Capabilities:
      node.role = event.role;
      node.format = event.format;
      node.executorApi = event.executorApi;
      break;
    case GraphNodeStatusPart.State:
      node.transferId = event.transferId;
      node.updateState = event.updateState;
      node.updateError = event.updateError;
      node.storeState = event.storeState;
      node.storeError = event.storeError;
      break;
    case GraphNodeStatusPart.Active:
      node.active = identity(event.generation, event.checksum);
      break;
    case GraphNodeStatusPart.Staged:
      node.staged = identity(event.generation, event.checksum);
      break;
    case GraphNodeStatusPart.Rollback:
      node.rollback = identity(event.generation, event.checksum);
      break;
    case GraphNodeStatusPart.Manifests:
      node.activeDevices = event.activeDevices;
      node.stagedDevices = event.stagedDevices;
      break;
    case GraphNodeStatusPart.RollbackManifest:
      node.rollbackDevices = event.rollbackDevices;
      break;
    case GraphNodeStatusPart.TransportDrops:
      node.rxDropped = event.rxDropped;
      node.txDropped = event.txDropped;
      break;
    case GraphNodeStatusPart.TransportErrors:
      node.busErrors = event.busErrors;
      break;
  }
  nodes.set(event.device, node);
}

function nodeReport(node: MutableNodeStatus): NetworkNodeReport {
  return {
    id: node.id,
    complete: node.parts === ALL_NODE_STATUS_PARTS,
    role: node.role,
    format: node.format,
    executorApi: node.executorApi,
    transferId: node.transferId,
    updateState: node.updateState,
    updateError: node.updateError,
    storeState: node.storeState,
    storeError: node.storeError,
    active: node.active,
    staged: node.staged,
    rollback: node.rollback,
    activeDevices: node.activeDevices,
    stagedDevices: node.stagedDevices,
    rollbackDevices: node.rollbackDevices,
    rxDropped: node.rxDropped,
    txDropped: node.txDropped,
    busErrors: node.busErrors,
  };
}

function sortedNodes(nodes: Map<number, MutableNodeStatus>): NetworkNodeReport[] {
  return [...nodes.values()].sort((left, right) => left.id - right.id).map(nodeReport);
}

export function devicesFromBitmap(bitmap: number): number[] {
  const devices: number[] = [];
  for (let device = 0; device < 32; device += 1) {
    if (((bitmap >>> device) & 1) !== 0) {
      devices.push(device);
    }
  }
  return devices;
}

function graphDeviceBitmap(graph: CompiledGraph): number {
  let bitmap = 0;
  for (const device of graph.devices) {
    bitmap = (bitmap | (2 ** device.id)) >>> 0;
  }
  return bitmap;
}

function graphRoles(graph: CompiledGraph): Map<number, DeviceRole> {
  return new Map(graph.devices.map((device) => [device.id, device.role]));
}

function sameIdentity(left: GraphIdentityReport | null, right: GraphIdentityReport): boolean {
  return left?.generation === right.generation && left.checksum === right.checksum;
}

function consensusIdentity(
  nodes: Map<number, MutableNodeStatus>,
  expectedDevices: readonly number[],
  select: (node: MutableNodeStatus) => GraphIdentityReport | null,
): GraphIdentityReport | null {
  let consensus: GraphIdentityReport | null = null;
  for (const device of expectedDevices) {
    const selected = nodes.get(device);
    const candidate = selected === undefined ? null : select(selected);
    if (candidate === null) {
      return null;
    }
    if (consensus === null) {
      consensus = candidate;
    } else if (!sameIdentity(candidate, consensus)) {
      return null;
    }
  }
  return consensus;
}

function operationTimeout(options?: { readonly timeoutMs?: number }): number {
  const timeoutMs = options?.timeoutMs ?? DEFAULT_OPERATION_TIMEOUT_MS;
  if (!Number.isInteger(timeoutMs) || timeoutMs < 100 || timeoutMs > 300_000) {
    throw new Error("timeout must be between 100 and 300000 milliseconds");
  }
  return timeoutMs;
}

async function issueCommand(
  client: PihoSerialClient,
  command: HostGraphCommand,
  operation: AckOperation,
  timeoutMs: number,
  onEvent: (event: DeviceEvent) => void,
): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  for (let attempt = 0; attempt < COMMAND_ATTEMPTS; attempt += 1) {
    await client.send(command);
    const remainingAttempts = COMMAND_ATTEMPTS - attempt;
    const remaining = deadline - Date.now();
    const attemptDeadline = attempt + 1 === COMMAND_ATTEMPTS
      ? deadline
      : Date.now() + Math.max(1, Math.floor(remaining / remainingAttempts));
    while (Date.now() < attemptDeadline) {
      const event = await client.nextEvent(Math.max(1, attemptDeadline - Date.now()));
      if (event === null) {
        break;
      }
      onEvent(event);
      if (event.kind === "ack" && event.operation === operation) {
        return event.accepted;
      }
    }
  }
  throw new Error(`timed out waiting for operation acknowledgement ${operation}`);
}

async function queryNodes(
  client: PihoSerialClient,
  expectedDevices: readonly number[] | null,
  timeoutMs: number,
): Promise<Map<number, MutableNodeStatus>> {
  const nodes = new Map<number, MutableNodeStatus>();
  const ingest = (event: DeviceEvent): void => {
    if (event.kind === "graphNodeStatus") {
      ingestNodeStatus(nodes, event);
    }
  };
  const accepted = await issueCommand(
    client,
    { kind: "graphStatus", transferId: 0 },
    AckOperation.GraphStatus,
    timeoutMs,
    ingest,
  );
  if (!accepted) {
    throw new Error("gateway rejected graph status request");
  }

  const deadline = Date.now() + timeoutMs;
  let quietDeadline = Date.now() + DISCOVERY_QUIET_MS;
  while (Date.now() < deadline) {
    if (
      expectedDevices !== null &&
      expectedDevices.every((device) => nodes.get(device)?.parts === ALL_NODE_STATUS_PARTS)
    ) {
      break;
    }
    const remaining = deadline - Date.now();
    const quietRemaining = quietDeadline - Date.now();
    if (expectedDevices === null && nodes.size !== 0 && quietRemaining <= 0) {
      break;
    }
    const event = await client.nextEvent(
      Math.max(1, Math.min(remaining, expectedDevices === null ? Math.max(1, quietRemaining) : remaining)),
    );
    if (event === null) {
      break;
    }
    if (event.kind === "graphNodeStatus") {
      ingestNodeStatus(nodes, event);
      quietDeadline = Date.now() + DISCOVERY_QUIET_MS;
    }
  }
  return nodes;
}

async function discoverNodes(
  client: PihoSerialClient,
  expectedDevices: readonly number[],
  timeoutMs: number,
): Promise<Map<number, MutableNodeStatus>> {
  const nodes = await queryNodes(client, null, timeoutMs);
  if (expectedDevices.some((device) =>
    nodes.get(device)?.parts !== ALL_NODE_STATUS_PARTS)) {
    const retry = await queryNodes(client, expectedDevices, timeoutMs);
    for (const [device, status] of retry) {
      nodes.set(device, status);
    }
  }
  return nodes;
}

const RECOVERABLE_STORAGE_ERRORS = new Set<GraphStoreError>([
  GraphStoreError.None,
  GraphStoreError.InvalidArgument,
  GraphStoreError.Busy,
  GraphStoreError.InvalidLength,
  GraphStoreError.InvalidChecksum,
  GraphStoreError.InvalidImage,
  GraphStoreError.Interrupted,
  GraphStoreError.NoStagedGraph,
  GraphStoreError.NoRollbackGraph,
  GraphStoreError.NoActiveGraph,
]);

function inspectNodes(
  nodes: Map<number, MutableNodeStatus>,
  expectedDevices: readonly number[],
  graph: CompiledGraph | null,
  expectedActive: GraphIdentityReport | null,
  expectedStaged: GraphIdentityReport | null,
): NetworkIssue[] {
  const issues: NetworkIssue[] = [];
  const expectedSet = new Set(expectedDevices);
  const unexpected = [...nodes.keys()]
    .filter((device) => !expectedSet.has(device))
    .sort((left, right) => left - right);
  if (unexpected.length !== 0) {
    issues.push({
      kind: "unexpected",
      devices: unexpected,
      detail: `unexpected devices: ${unexpected.join(", ")}`,
      severity: "error",
    });
  }
  const expectedBitmap = expectedDevices.reduce(
    (bitmap, device) => (bitmap | (2 ** device)) >>> 0,
    0,
  );
  const missing = expectedDevices.filter((device) => !nodes.has(device));
  if (missing.length !== 0) {
    issues.push({
      kind: "missing",
      devices: missing,
      detail: `missing devices: ${missing.join(", ")}`,
      severity: "error",
    });
  }
  const roles = graph === null ? new Map<number, DeviceRole>() : graphRoles(graph);
  for (const device of expectedDevices) {
    const node = nodes.get(device);
    if (node === undefined) {
      continue;
    }
    if (node.parts !== ALL_NODE_STATUS_PARTS) {
      issues.push({
        kind: "incomplete_status",
        devices: [device],
        detail: `device ${device} returned incomplete status`,
        severity: "error",
      });
      continue;
    }
    const expectedRole = roles.get(device);
    if (expectedRole !== undefined && node.role !== expectedRole) {
      issues.push({
        kind: "wrong_role",
        devices: [device],
        detail: `device ${device} is ${node.role ?? "unknown"}; graph requires ${expectedRole}`,
        severity: "error",
      });
    }
    if (graph !== null && node.format !== graph.format) {
      issues.push({
        kind: "incompatible_format",
        devices: [device],
        detail: `device ${device} supports graph format ${node.format ?? "unknown"}; graph requires ${graph.format}`,
        severity: "error",
      });
    }
    if (graph !== null && (node.executorApi === null || node.executorApi < graph.executorApi)) {
      issues.push({
        kind: "incompatible_executor",
        devices: [device],
        detail: `device ${device} supports executor API ${node.executorApi ?? "unknown"}; graph requires ${graph.executorApi}`,
        severity: "error",
      });
    }
    if (node.storeError !== GraphStoreError.None) {
      issues.push({
        kind: "storage",
        devices: [device],
        detail: `device ${device} reports storage error ${GraphStoreError[node.storeError] ?? node.storeError}`,
        severity: RECOVERABLE_STORAGE_ERRORS.has(node.storeError) ? "warning" : "error",
      });
    }
    if (node.rxDropped !== 0 || node.txDropped !== 0 || node.busErrors !== 0) {
      issues.push({
        kind: "transport",
        devices: [device],
        detail: `device ${device} transport counters: rx_dropped=${node.rxDropped}, tx_dropped=${node.txDropped}, bus_errors=${node.busErrors}`,
        severity: "warning",
      });
    }
    if (node.updateError !== GraphUpdateError.None) {
      issues.push({
        kind: "rejected",
        devices: [device],
        detail: `device ${device} reports graph update error ${GraphUpdateError[node.updateError] ?? node.updateError}`,
        severity: "warning",
      });
    }
    if (expectedActive !== null &&
        (!sameIdentity(node.active, expectedActive) ||
         node.activeDevices !== expectedBitmap)) {
      issues.push({
        kind: "active_mismatch",
        devices: [device],
        detail: `device ${device} does not report the expected active graph and manifest`,
        severity: "error",
      });
    }
    if (expectedStaged !== null &&
        (!sameIdentity(node.staged, expectedStaged) ||
         node.stagedDevices !== expectedBitmap)) {
      issues.push({
        kind: "staged_mismatch",
        devices: [device],
        detail: `device ${device} does not report the expected staged graph and manifest`,
        severity: "error",
      });
    }
  }
  return issues;
}

function statusReport(
  nodes: Map<number, MutableNodeStatus>,
  expectedDevices: readonly number[],
  graph: CompiledGraph | null,
  expectedActive: GraphIdentityReport | null,
): NetworkStatusReport {
  const issues = inspectNodes(nodes, expectedDevices, graph, expectedActive, null);
  const activeAgreement = consensusIdentity(nodes, expectedDevices, (node) => node.active);
  if (expectedDevices.length !== 0 && activeAgreement === null && expectedActive === null) {
    issues.push({
      kind: "active_mismatch",
      devices: expectedDevices,
      detail: "expected devices do not agree on one active generation and checksum",
      severity: "error",
    });
  }
  if (expectedDevices.length === 0) {
    issues.push({
      kind: "missing",
      devices: [],
      detail: "no graph devices were discovered",
      severity: "error",
    });
  }
  const expectedBitmap = expectedDevices.reduce(
    (bitmap, device) => (bitmap | (2 ** device)) >>> 0,
    0,
  );
  const manifestMismatch = expectedDevices.filter((device) => {
    const node = nodes.get(device);
    return node !== undefined && node.parts === ALL_NODE_STATUS_PARTS &&
      node.activeDevices !== expectedBitmap;
  });
  if (expectedActive === null && manifestMismatch.length !== 0) {
    issues.push({
      kind: "active_mismatch",
      devices: manifestMismatch,
      detail: "active graph device manifests do not agree with the expected network",
      severity: "error",
    });
  }
  const discoveredDevices = [...nodes.keys()].sort((left, right) => left - right);
  const missingDevices = expectedDevices.filter((device) => !nodes.has(device));
  return {
    operation: "status",
    ok: issues.every((issue) => issue.severity !== "error"),
    expectedDevices,
    discoveredDevices,
    missingDevices,
    activeAgreement,
    nodes: sortedNodes(nodes),
    issues,
  };
}

function expectedFromInventory(nodes: Map<number, MutableNodeStatus>): number[] {
  const manifests = [...nodes.values()]
    .filter((node) => node.parts === ALL_NODE_STATUS_PARTS && node.activeDevices !== 0)
    .map((node) => node.activeDevices);
  if (manifests.length === 0) {
    return [...nodes.keys()].sort((left, right) => left - right);
  }
  const first = manifests[0];
  if (first === undefined) {
    return [];
  }
  const bitmap = manifests.reduce((combined, manifest) => (combined | manifest) >>> 0, 0);
  return devicesFromBitmap(bitmap);
}

export async function readNetworkStatus(
  client: PihoSerialClient,
  image: Uint8Array | null,
  options: { readonly timeoutMs?: number } = {},
): Promise<NetworkStatusReport> {
  const timeoutMs = operationTimeout(options);
  if (image !== null) {
    const graph = decodeGraphImage(image);
    const expectedDevices = graph.devices.map((device) => device.id);
    const nodes = await discoverNodes(client, expectedDevices, timeoutMs);
    const checksum = new DataView(image.buffer, image.byteOffset, image.byteLength)
      .getUint32(GRAPH_CHECKSUM_OFFSET, true);
    return statusReport(nodes, expectedDevices, graph, { generation: graph.generation, checksum });
  }
  const nodes = await queryNodes(client, null, timeoutMs);
  const expectedDevices = expectedFromInventory(nodes);
  if (expectedDevices.some((device) =>
    nodes.get(device)?.parts !== ALL_NODE_STATUS_PARTS)) {
    const retry = await queryNodes(client, expectedDevices, timeoutMs);
    for (const [device, status] of retry) {
      nodes.set(device, status);
    }
  }
  return statusReport(nodes, expectedDevices, null, null);
}

function deterministicTransferId(identity_: GraphIdentityReport, imageSize: number): number {
  let value = identity_.generation ^ (identity_.generation >>> 16) ^ identity_.checksum ^
    (identity_.checksum >>> 16) ^ imageSize;
  value &= 0xffff;
  return value === 0 ? 1 : value;
}

function rejectedIssue(update: GraphUpdateDeviceEvent): NetworkIssue {
  const rejected = devicesFromBitmap(update.rejectedDevices);
  return {
    kind: "rejected",
    devices: rejected,
    detail: `devices ${rejected.join(", ")} rejected graph update with ${GraphUpdateError[update.error] ?? update.error}`,
    severity: "error",
  };
}

async function waitForUpdate(
  client: PihoSerialClient,
  transferId: number | null,
  timeoutMs: number,
  predicate: (event: GraphUpdateDeviceEvent) => boolean,
  onEvent: (event: DeviceEvent) => void,
  initial: GraphUpdateDeviceEvent | null = null,
): Promise<GraphUpdateDeviceEvent> {
  const deadline = Date.now() + timeoutMs;
  let latest = initial !== null &&
    (transferId === null || initial.transferId === transferId) ? initial : null;
  if (latest !== null &&
      (latest.state === GraphUpdateState.Rejected ||
       latest.rejectedDevices !== 0 ||
       predicate(latest))) {
    return latest;
  }
  while (Date.now() < deadline) {
    const event = await client.nextEvent(Math.max(1, deadline - Date.now()));
    if (event === null) {
      break;
    }
    onEvent(event);
    if (event.kind !== "graphUpdate" ||
        (transferId !== null && event.transferId !== transferId)) {
      continue;
    }
    latest = event;
    if (event.state === GraphUpdateState.Rejected || event.rejectedDevices !== 0) {
      return event;
    }
    if (predicate(event)) {
      return event;
    }
  }
  if (latest !== null) {
    return latest;
  }
  throw new Error("timed out waiting for graph update status");
}

function deploymentFailure(
  phase: DeploymentProgress["phase"],
  identity_: GraphIdentityReport,
  expectedDevices: readonly number[],
  totalChunks: number,
  completedChunks: number,
  nodes: Map<number, MutableNodeStatus>,
  issues: readonly NetworkIssue[],
  update: GraphUpdateDeviceEvent | null,
): DeploymentReport {
  return {
    operation: "deploy",
    ok: false,
    phase,
    activated: false,
    identity: identity_,
    expectedDevices,
    missingDevices: update === null ? [] : devicesFromBitmap(update.missingDevices),
    rejectedDevices: update === null ? [] : devicesFromBitmap(update.rejectedDevices),
    completedChunks,
    totalChunks,
    nodes: sortedNodes(nodes),
    issues,
  };
}

export async function deployGraph(
  client: PihoSerialClient,
  image: Uint8Array,
  options: DeploymentOptions = {},
): Promise<DeploymentReport> {
  const timeoutMs = operationTimeout(options);
  const graph = decodeGraphImage(image);
  const expectedBitmap = graphDeviceBitmap(graph);
  const expectedDevices = graph.devices.map((device) => device.id);
  const checksum = new DataView(image.buffer, image.byteOffset, image.byteLength)
    .getUint32(GRAPH_CHECKSUM_OFFSET, true);
  const graphIdentity = { generation: graph.generation, checksum };
  const transferId = deterministicTransferId(graphIdentity, image.length);
  const totalChunks = Math.ceil(image.length / GRAPH_CHUNK_BYTES);
  let completedChunks = 0;
  let phase: DeploymentProgress["phase"] = "preflight";
  let latestUpdate: GraphUpdateDeviceEvent | null = null;
  let nodes = new Map<number, MutableNodeStatus>();
  const onEvent = (event: DeviceEvent): void => {
    if (event.kind === "graphUpdate" && event.transferId === transferId) {
      latestUpdate = event;
    } else if (event.kind === "graphNodeStatus") {
      ingestNodeStatus(nodes, event);
    }
  };
  const progress = (): void => options.onProgress?.({ phase, completedChunks, totalChunks });

  try {
    nodes = await discoverNodes(client, expectedDevices, timeoutMs);
    let issues = inspectNodes(nodes, expectedDevices, graph, null, null);
    const blocking = issues.filter((issue) => issue.severity === "error");
    if (blocking.length !== 0) {
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, null);
    }
    progress();

    const staleTransfers = [...new Set(
      expectedDevices
        .map((device) => nodes.get(device))
        .filter((node): node is MutableNodeStatus => node !== undefined)
        .filter((node) => node.transferId !== 0 &&
          (node.updateState === GraphUpdateState.Receiving ||
           node.updateState === GraphUpdateState.Validating ||
           node.updateState === GraphUpdateState.Staged))
        .map((node) => node.transferId),
    )];
    for (const staleTransfer of staleTransfers) {
      const aborted = await issueCommand(
        client,
        { kind: "graphAbort", transferId: staleTransfer },
        AckOperation.GraphAbort,
        timeoutMs,
        onEvent,
      );
      if (!aborted) {
        issues = [{
          kind: "rejected",
          devices: [],
          detail: `gateway refused to abort stale transfer ${staleTransfer}`,
          severity: "error",
        }];
        return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                                 completedChunks, nodes, issues, latestUpdate);
      }
    }

    const descriptor = {
      transferId,
      generation: graph.generation,
      imageSize: image.length,
      format: graph.format,
      executorApi: graph.executorApi,
      expectedDevices: expectedBitmap,
      checksum,
    };
    if (!await issueCommand(client, { kind: "graphBegin", descriptor },
                            AckOperation.GraphBegin, timeoutMs, onEvent)) {
      issues = [{
        kind: "rejected",
        devices: [],
        detail: "gateway rejected graph begin",
        severity: "error",
      }];
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, latestUpdate);
    }
    let update = await waitForUpdate(
      client,
      transferId,
      timeoutMs,
      (event) => event.readyDevices === expectedBitmap,
      onEvent,
      latestUpdate,
    );
    latestUpdate = update;
    if (update.readyDevices !== expectedBitmap || update.rejectedDevices !== 0) {
      issues = update.rejectedDevices !== 0
        ? [rejectedIssue(update)]
        : [{ kind: "timeout", devices: devicesFromBitmap(update.missingDevices),
             detail: "not every expected device became ready", severity: "error" }];
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, update);
    }

    phase = "transfer";
    progress();
    for (let sequence = 0; sequence < totalChunks; sequence += 1) {
      const offset = sequence * GRAPH_CHUNK_BYTES;
      const data = image.subarray(offset, Math.min(offset + GRAPH_CHUNK_BYTES, image.length));
      if (!await issueCommand(
        client,
        { kind: "graphChunk", transferId, sequence, data },
        AckOperation.GraphChunk,
        timeoutMs,
        onEvent,
      )) {
        issues = [{ kind: "rejected", devices: [],
                    detail: `gateway rejected graph chunk ${sequence}`, severity: "error" }];
        return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                                 completedChunks, nodes, issues, latestUpdate);
      }
      update = await waitForUpdate(
        client,
        transferId,
        timeoutMs,
        (event) => !event.chunkPending && event.nextSequence >= sequence + 1,
        onEvent,
        latestUpdate,
      );
      latestUpdate = update;
      if (update.rejectedDevices !== 0 || update.nextSequence < sequence + 1) {
        issues = update.rejectedDevices !== 0
          ? [rejectedIssue(update)]
          : [{ kind: "timeout", devices: devicesFromBitmap(update.missingDevices),
               detail: `chunk ${sequence} did not converge`, severity: "error" }];
        return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                                 completedChunks, nodes, issues, update);
      }
      completedChunks = sequence + 1;
      progress();
    }

    if (!await issueCommand(
      client,
      { kind: "graphFinish", transferId, sequenceCount: totalChunks },
      AckOperation.GraphFinish,
      timeoutMs,
      onEvent,
    )) {
      issues = [{ kind: "rejected", devices: [], detail: "gateway rejected graph finish",
                  severity: "error" }];
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, latestUpdate);
    }
    update = await waitForUpdate(
      client,
      transferId,
      timeoutMs,
      (event) => event.state === GraphUpdateState.Staged && event.stagedDevices === expectedBitmap,
      onEvent,
      latestUpdate,
    );
    latestUpdate = update;
    if (update.state !== GraphUpdateState.Staged || update.stagedDevices !== expectedBitmap) {
      issues = update.rejectedDevices !== 0
        ? [rejectedIssue(update)]
        : [{ kind: "timeout", devices: devicesFromBitmap(update.missingDevices),
             detail: "not every expected device staged the graph", severity: "error" }];
      return deploymentFailure("staged", graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, update);
    }
    phase = "staged";
    progress();

    phase = "activation";
    progress();
    if (!await issueCommand(client, { kind: "graphActivate" }, AckOperation.GraphActivate,
                            timeoutMs, onEvent)) {
      issues = [{ kind: "rejected", devices: [], detail: "gateway refused graph activation",
                  severity: "error" }];
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, latestUpdate);
    }
    update = await waitForUpdate(
      client,
      transferId,
      timeoutMs,
      (event) => event.state === GraphUpdateState.Active && event.activeDevices === expectedBitmap,
      onEvent,
      latestUpdate,
    );
    latestUpdate = update;
    if (update.state !== GraphUpdateState.Active || update.activeDevices !== expectedBitmap) {
      issues = update.rejectedDevices !== 0
        ? [rejectedIssue(update)]
        : [{ kind: "timeout", devices: devicesFromBitmap(update.missingDevices),
             detail: "not every expected device activated the graph", severity: "error" }];
      return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                               completedChunks, nodes, issues, update);
    }

    nodes = await discoverNodes(client, expectedDevices, timeoutMs);
    issues = inspectNodes(nodes, expectedDevices, graph, graphIdentity, null);
    phase = "complete";
    progress();
    return {
      operation: "deploy",
      ok: issues.every((issue) => issue.severity !== "error"),
      phase,
      activated: true,
      identity: graphIdentity,
      expectedDevices,
      missingDevices: expectedDevices.filter((device) => !nodes.has(device)),
      rejectedDevices: [],
      completedChunks,
      totalChunks,
      nodes: sortedNodes(nodes),
      issues,
    };
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    const issues: NetworkIssue[] = [{
      kind: detail.includes("timed out") ? "timeout" : "transport",
      devices: latestUpdate === null ? [] : devicesFromBitmap(latestUpdate.missingDevices),
      detail,
      severity: "error",
    }];
    return deploymentFailure(phase, graphIdentity, expectedDevices, totalChunks,
                             completedChunks, nodes, issues, latestUpdate);
  }
}

export async function rollbackGraph(
  client: PihoSerialClient,
  options: { readonly timeoutMs?: number } = {},
): Promise<RollbackReport> {
  const timeoutMs = operationTimeout(options);
  let nodes = new Map<number, MutableNodeStatus>();
  let expectedDevices: number[] = [];
  let latestUpdate: GraphUpdateDeviceEvent | null = null;
  const onEvent = (event: DeviceEvent): void => {
    if (event.kind === "graphUpdate") {
      latestUpdate = event;
    } else if (event.kind === "graphNodeStatus") {
      ingestNodeStatus(nodes, event);
    }
  };
  try {
    nodes = await queryNodes(client, null, timeoutMs);
    expectedDevices = expectedFromInventory(nodes);
    if (expectedDevices.some((device) =>
      nodes.get(device)?.parts !== ALL_NODE_STATUS_PARTS)) {
      const retry = await queryNodes(client, expectedDevices, timeoutMs);
      for (const [device, status] of retry) {
        nodes.set(device, status);
      }
    }
    const issues = inspectNodes(nodes, expectedDevices, null, null, null);
    const active = consensusIdentity(nodes, expectedDevices, (node) => node.active);
    const target = consensusIdentity(nodes, expectedDevices, (node) => node.rollback);
    const rollbackManifests = expectedDevices.map((device) => nodes.get(device)?.rollbackDevices ?? 0);
    const expectedBitmap = expectedDevices.reduce(
      (bitmap, device) => (bitmap | (2 ** device)) >>> 0,
      0,
    );
    if (active === null) {
      issues.push({ kind: "active_mismatch", devices: expectedDevices,
                    detail: "expected devices do not agree on the active graph",
                    severity: "error" });
    }
    if (target === null || target.generation === 0) {
      issues.push({ kind: "rollback_mismatch", devices: expectedDevices,
                    detail: "expected devices do not agree on a retained rollback graph",
                    severity: "error" });
    }
    if (rollbackManifests.some((bitmap) => bitmap !== expectedBitmap)) {
      issues.push({ kind: "rollback_mismatch", devices: expectedDevices,
                    detail: "rollback graph device manifests do not match the active network",
                    severity: "error" });
    }
    if (issues.some((issue) => issue.severity === "error") || target === null) {
      return {
        operation: "rollback",
        ok: false,
        identity: target,
        expectedDevices,
        missingDevices: expectedDevices.filter((device) => !nodes.has(device)),
        rejectedDevices: [],
        nodes: sortedNodes(nodes),
        issues,
      };
    }

    const accepted = await issueCommand(
      client,
      { kind: "graphRollback", generation: target.generation, checksum: target.checksum,
        expectedDevices: expectedBitmap },
      AckOperation.GraphRollback,
      timeoutMs,
      onEvent,
    );
    if (!accepted) {
      issues.push({ kind: "rejected", devices: [], detail: "gateway refused rollback",
                    severity: "error" });
    } else {
      const update = await waitForUpdate(
        client,
        null,
        timeoutMs,
        (event) => event.state === GraphUpdateState.Rollback &&
          event.rollbackDevices === expectedBitmap,
        onEvent,
        latestUpdate,
      );
      latestUpdate = update;
      if (update.state !== GraphUpdateState.Rollback || update.rollbackDevices !== expectedBitmap) {
        issues.push(update.rejectedDevices !== 0
          ? rejectedIssue(update)
          : { kind: "timeout", devices: devicesFromBitmap(update.missingDevices),
              detail: "rollback did not converge", severity: "error" });
      }
    }
    nodes = await discoverNodes(client, expectedDevices, timeoutMs);
    issues.push(...inspectNodes(nodes, expectedDevices, null, target, null));
    return {
      operation: "rollback",
      ok: issues.every((issue) => issue.severity !== "error"),
      identity: target,
      expectedDevices,
      missingDevices: expectedDevices.filter((device) => !nodes.has(device)),
      rejectedDevices: latestUpdate === null ? [] : devicesFromBitmap(latestUpdate.rejectedDevices),
      nodes: sortedNodes(nodes),
      issues,
    };
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    return {
      operation: "rollback",
      ok: false,
      identity: null,
      expectedDevices,
      missingDevices: latestUpdate === null ? [] : devicesFromBitmap(latestUpdate.missingDevices),
      rejectedDevices: latestUpdate === null ? [] : devicesFromBitmap(latestUpdate.rejectedDevices),
      nodes: sortedNodes(nodes),
      issues: [{ kind: detail.includes("timed out") ? "timeout" : "transport", devices: [],
                 detail, severity: "error" }],
    };
  }
}
