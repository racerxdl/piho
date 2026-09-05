import { describe, expect, test } from "bun:test";
import { join } from "node:path";
import {
  deployGraph,
  readNetworkStatus,
  rollbackGraph,
  type GraphIdentityReport,
} from "../src/deploy.ts";
import { decodeGraphImage, GRAPH_CHECKSUM_OFFSET } from "../src/image.ts";
import {
  AckOperation,
  decodeHostCommand,
  encodeDeviceEvent,
  encodeSerialFrame,
  GraphNodeStatusPart,
  GraphStoreError,
  GraphStoreState,
  GraphUpdateError,
  GraphUpdateState,
  SerialFrameDecoder,
  type DeviceEvent,
  type DeviceRole,
  type GraphDescriptor,
  type GraphNodeStatusDeviceEvent,
  type GraphUpdateDeviceEvent,
  type HostGraphCommand,
} from "../src/protocol.ts";
import { PihoSerialClient, type ByteTransport } from "../src/serial.ts";

interface SimulatedNode {
  readonly id: number;
  role: DeviceRole;
  format: number;
  executorApi: number;
  transferId: number;
  updateState: GraphUpdateState;
  updateError: GraphUpdateError;
  storeState: GraphStoreState;
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

interface SimulatedFailure {
  readonly sequence: number;
  readonly missingDevice: number;
  readonly rejectedDevice: number | null;
  readonly error: GraphUpdateError;
}

interface SimulatorOptions {
  readonly omit?: readonly number[];
  readonly roles?: Readonly<Record<number, DeviceRole>>;
  readonly formats?: Readonly<Record<number, number>>;
  readonly executorApis?: Readonly<Record<number, number>>;
  readonly extra?: readonly { readonly id: number; readonly role: DeviceRole }[];
  readonly storageErrors?: Readonly<Record<number, GraphStoreError>>;
  readonly transport?: Readonly<Record<number, {
    readonly rxDropped: number;
    readonly txDropped: number;
    readonly busErrors: number;
  }>>;
}


const fixturePath = join(import.meta.dir, "fixtures/synthetic.phg");

function identity(generation: number, checksum: number): GraphIdentityReport {
  return { generation, checksum };
}

function sameIdentity(
  left: GraphIdentityReport | null,
  right: GraphIdentityReport,
): boolean {
  return left?.generation === right.generation && left.checksum === right.checksum;
}

function bitmap(devices: readonly number[]): number {
  return devices.reduce((result, device) => (result | (2 ** device)) >>> 0, 0);
}

function checksum(image: Uint8Array): number {
  return new DataView(image.buffer, image.byteOffset, image.byteLength)
    .getUint32(GRAPH_CHECKSUM_OFFSET, true);
}

function equalBytes(left: Uint8Array, right: Uint8Array): boolean {
  return left.length === right.length && left.every((value, index) => value === right[index]);
}

class SimulatedNetworkTransport implements ByteTransport {
  readonly commands: HostGraphCommand[] = [];
  duplicateChunks = 0;
  failure: SimulatedFailure | null = null;

  private readonly input = new SerialFrameDecoder();
  private readonly queued: Uint8Array[] = [];
  private readonly nodes = new Map<number, SimulatedNode>();
  private descriptor: GraphDescriptor | null = null;
  private readonly chunks = new Map<number, Uint8Array>();
  private readonly dropAckKinds = new Set<HostGraphCommand["kind"]>();
  private closed = false;

  constructor(private readonly image: Uint8Array, options: SimulatorOptions = {}) {
    const graph = decodeGraphImage(image);
    const ids = graph.devices.map((device) => device.id);
    const deviceBitmap = bitmap(ids);
    const omitted = new Set(options.omit ?? []);
    const devices = [
      ...graph.devices.map((device) => ({ id: device.id, role: device.role })),
      ...(options.extra ?? []),
    ];
    for (const device of devices) {
      if (omitted.has(device.id)) {
        continue;
      }
      const counters = options.transport?.[device.id];
      this.nodes.set(device.id, {
        id: device.id,
        role: options.roles?.[device.id] ?? device.role,
        format: options.formats?.[device.id] ?? graph.format,
        executorApi: options.executorApis?.[device.id] ?? graph.executorApi,
        transferId: 0,
        updateState: GraphUpdateState.Active,
        updateError: GraphUpdateError.None,
        storeState: GraphStoreState.Active,
        storeError: options.storageErrors?.[device.id] ?? GraphStoreError.None,
        active: identity(42, 0x1020_3040),
        staged: null,
        rollback: identity(41, 0x5060_7080),
        activeDevices: deviceBitmap,
        stagedDevices: 0,
        rollbackDevices: deviceBitmap,
        rxDropped: counters?.rxDropped ?? 0,
        txDropped: counters?.txDropped ?? 0,
        busErrors: counters?.busErrors ?? 0,
      });
    }
  }

  dropNextAck(kind: HostGraphCommand["kind"]): void {
    this.dropAckKinds.add(kind);
  }

  activeIdentities(): GraphIdentityReport[] {
    return [...this.nodes.values()].flatMap((node) => node.active === null ? [] : [node.active]);
  }

  private deliver(chunk: Uint8Array): void {
    this.queued.push(chunk);
  }

  private emit(event: DeviceEvent): void {
    this.deliver(encodeSerialFrame(encodeDeviceEvent(event)));
  }

  private ack(command: HostGraphCommand, operation: AckOperation, accepted = true): void {
    if (this.dropAckKinds.delete(command.kind)) {
      return;
    }
    this.emit({ kind: "ack", operation, accepted });
  }

  private expectedDevices(): number {
    return this.descriptor?.expectedDevices ?? bitmap([...this.nodes.keys()]);
  }

  private update(overrides: Partial<GraphUpdateDeviceEvent> = {}): void {
    const descriptor = this.descriptor;
    const expectedDevices = descriptor?.expectedDevices ?? this.expectedDevices();
    this.emit({
      kind: "graphUpdate",
      transferId: descriptor?.transferId ?? 0,
      generation: descriptor?.generation ?? 0,
      checksum: descriptor?.checksum ?? 0,
      expectedDevices,
      readyDevices: 0,
      progressedDevices: 0,
      stagedDevices: 0,
      rejectedDevices: 0,
      activeDevices: 0,
      rollbackDevices: 0,
      missingDevices: expectedDevices,
      nextSequence: this.chunks.size,
      sequenceCount: descriptor === null ? 0 : Math.ceil(descriptor.imageSize / 4),
      state: GraphUpdateState.Idle,
      error: GraphUpdateError.None,
      chunkPending: false,
      ...overrides,
    });
  }

  private nodeEvent(node: SimulatedNode, part: GraphNodeStatusPart): GraphNodeStatusDeviceEvent {
    let generation = 0;
    let imageChecksum = 0;
    if (part === GraphNodeStatusPart.Active && node.active !== null) {
      ({ generation, checksum: imageChecksum } = node.active);
    } else if (part === GraphNodeStatusPart.Staged && node.staged !== null) {
      ({ generation, checksum: imageChecksum } = node.staged);
    } else if (part === GraphNodeStatusPart.Rollback && node.rollback !== null) {
      ({ generation, checksum: imageChecksum } = node.rollback);
    }
    return {
      kind: "graphNodeStatus",
      device: node.id,
      part,
      format: part === GraphNodeStatusPart.Capabilities ? node.format : 0,
      executorApi: part === GraphNodeStatusPart.Capabilities ? node.executorApi : 0,
      role: part === GraphNodeStatusPart.Capabilities ? node.role : null,
      transferId: part === GraphNodeStatusPart.State ? node.transferId : 0,
      updateState: part === GraphNodeStatusPart.State ? node.updateState : GraphUpdateState.Idle,
      updateError: part === GraphNodeStatusPart.State ? node.updateError : GraphUpdateError.None,
      storeState: part === GraphNodeStatusPart.State ? node.storeState : GraphStoreState.Empty,
      storeError: part === GraphNodeStatusPart.State ? node.storeError : GraphStoreError.None,
      generation,
      checksum: imageChecksum,
      activeDevices: part === GraphNodeStatusPart.Manifests ? node.activeDevices : 0,
      stagedDevices: part === GraphNodeStatusPart.Manifests ? node.stagedDevices : 0,
      rollbackDevices: part === GraphNodeStatusPart.RollbackManifest ? node.rollbackDevices : 0,
      rxDropped: part === GraphNodeStatusPart.TransportDrops ? node.rxDropped : 0,
      txDropped: part === GraphNodeStatusPart.TransportDrops ? node.txDropped : 0,
      busErrors: part === GraphNodeStatusPart.TransportErrors ? node.busErrors : 0,
    };
  }

  private publishInventory(): void {
    for (const node of this.nodes.values()) {
      for (let part = GraphNodeStatusPart.Capabilities;
           part <= GraphNodeStatusPart.TransportErrors; part += 1) {
        this.emit(this.nodeEvent(node, part));
      }
    }
  }

  private resetTransfer(descriptor: GraphDescriptor): void {
    this.descriptor = descriptor;
    this.chunks.clear();
    for (const node of this.nodes.values()) {
      node.transferId = descriptor.transferId;
      node.updateState = GraphUpdateState.Receiving;
      node.updateError = GraphUpdateError.None;
      node.storeState = GraphStoreState.Receiving;
      node.storeError = GraphStoreError.None;
      node.staged = null;
      node.stagedDevices = 0;
    }
  }

  private assembledImage(): Uint8Array {
    if (this.descriptor === null) {
      return new Uint8Array();
    }
    const assembled = new Uint8Array(this.descriptor.imageSize);
    for (const [sequence, data] of this.chunks) {
      assembled.set(data, sequence * 4);
    }
    return assembled;
  }

  private handleBegin(command: Extract<HostGraphCommand, { kind: "graphBegin" }>): void {
    const duplicate = this.descriptor?.transferId === command.descriptor.transferId &&
      this.descriptor.generation === command.descriptor.generation &&
      this.descriptor.checksum === command.descriptor.checksum;
    if (!duplicate) {
      this.resetTransfer(command.descriptor);
    }
    this.ack(command, AckOperation.GraphBegin);
    this.update({
      readyDevices: command.descriptor.expectedDevices,
      missingDevices: 0,
      state: GraphUpdateState.Receiving,
    });
  }

  private handleChunk(command: Extract<HostGraphCommand, { kind: "graphChunk" }>): void {
    if (this.descriptor === null || command.transferId !== this.descriptor.transferId) {
      this.ack(command, AckOperation.GraphChunk, false);
      return;
    }
    const prior = this.chunks.get(command.sequence);
    if (prior !== undefined) {
      if (!equalBytes(prior, command.data)) {
        this.ack(command, AckOperation.GraphChunk, false);
        return;
      }
      this.duplicateChunks += 1;
    } else if (command.sequence !== this.chunks.size) {
      this.ack(command, AckOperation.GraphChunk, false);
      return;
    } else if (this.failure?.sequence !== command.sequence) {
      this.chunks.set(command.sequence, command.data.slice());
    }
    this.ack(command, AckOperation.GraphChunk);

    if (this.failure?.sequence === command.sequence) {
      const failure = this.failure;
      const missing = (2 ** failure.missingDevice) >>> 0;
      const rejected = failure.rejectedDevice === null
        ? 0
        : (2 ** failure.rejectedDevice) >>> 0;
      for (const node of this.nodes.values()) {
        node.updateState = failure.rejectedDevice !== null &&
          node.id === failure.rejectedDevice
          ? GraphUpdateState.Rejected
          : GraphUpdateState.Receiving;
        node.updateError = failure.rejectedDevice !== null &&
          node.id === failure.rejectedDevice ? failure.error : GraphUpdateError.None;
      }
      this.update({
        readyDevices: this.descriptor.expectedDevices,
        progressedDevices: this.descriptor.expectedDevices & ~(missing | rejected),
        rejectedDevices: rejected,
        missingDevices: missing,
        state: rejected === 0 ? GraphUpdateState.Receiving : GraphUpdateState.Rejected,
        error: rejected === 0 ? GraphUpdateError.None : failure.error,
        chunkPending: true,
      });
      return;
    }
    this.update({
      readyDevices: this.descriptor.expectedDevices,
      progressedDevices: this.descriptor.expectedDevices,
      missingDevices: 0,
      nextSequence: this.chunks.size,
      state: GraphUpdateState.Receiving,
    });
  }

  private handleFinish(command: Extract<HostGraphCommand, { kind: "graphFinish" }>): void {
    if (this.descriptor === null || command.transferId !== this.descriptor.transferId ||
        command.sequenceCount !== this.chunks.size || !equalBytes(this.assembledImage(), this.image) ||
        checksum(this.image) !== this.descriptor.checksum) {
      this.ack(command, AckOperation.GraphFinish, false);
      return;
    }
    const graphIdentity = identity(this.descriptor.generation, this.descriptor.checksum);
    for (const node of this.nodes.values()) {
      node.updateState = GraphUpdateState.Staged;
      node.storeState = GraphStoreState.Staged;
      node.staged = graphIdentity;
      node.stagedDevices = this.descriptor.expectedDevices;
    }
    this.ack(command, AckOperation.GraphFinish);
    this.update({
      readyDevices: this.descriptor.expectedDevices,
      stagedDevices: this.descriptor.expectedDevices,
      missingDevices: 0,
      nextSequence: this.chunks.size,
      state: GraphUpdateState.Staged,
    });
  }

  private handleActivate(command: Extract<HostGraphCommand, { kind: "graphActivate" }>): void {
    if (this.descriptor === null) {
      this.ack(command, AckOperation.GraphActivate, false);
      return;
    }
    const target = identity(this.descriptor.generation, this.descriptor.checksum);
    if ([...this.nodes.values()].every((node) => sameIdentity(node.active, target))) {
      this.ack(command, AckOperation.GraphActivate);
      this.update({
        activeDevices: this.descriptor.expectedDevices,
        missingDevices: 0,
        nextSequence: this.chunks.size,
        state: GraphUpdateState.Active,
      });
      return;
    }
    if ([...this.nodes.values()].some((node) => node.staged === null)) {
      this.ack(command, AckOperation.GraphActivate, false);
      return;
    }
    for (const node of this.nodes.values()) {
      node.rollback = node.active;
      node.rollbackDevices = node.activeDevices;
      node.active = node.staged;
      node.activeDevices = node.stagedDevices;
      node.staged = null;
      node.stagedDevices = 0;
      node.updateState = GraphUpdateState.Active;
      node.storeState = GraphStoreState.Active;
    }
    this.ack(command, AckOperation.GraphActivate);
    this.update({
      activeDevices: this.descriptor.expectedDevices,
      missingDevices: 0,
      nextSequence: this.chunks.size,
      state: GraphUpdateState.Active,
    });
  }

  private handleRollback(command: Extract<HostGraphCommand, { kind: "graphRollback" }>): void {
    const target = identity(command.generation, command.checksum);
    if (command.expectedDevices !== this.expectedDevices() ||
        [...this.nodes.values()].some((node) => !sameIdentity(node.rollback, target))) {
      this.ack(command, AckOperation.GraphRollback, false);
      return;
    }
    for (const node of this.nodes.values()) {
      const previous = node.active;
      node.active = node.rollback;
      node.rollback = previous;
      node.updateState = GraphUpdateState.Rollback;
      node.storeState = GraphStoreState.Rollback;
    }
    this.ack(command, AckOperation.GraphRollback);
    this.update({
      generation: target.generation,
      checksum: target.checksum,
      rollbackDevices: command.expectedDevices,
      missingDevices: 0,
      state: GraphUpdateState.Rollback,
    });
  }

  private handleAbort(command: Extract<HostGraphCommand, { kind: "graphAbort" }>): void {
    for (const node of this.nodes.values()) {
      if (node.transferId === command.transferId) {
        node.transferId = 0;
        node.updateState = GraphUpdateState.Active;
        node.updateError = GraphUpdateError.Aborted;
        node.storeState = GraphStoreState.Active;
        node.staged = null;
        node.stagedDevices = 0;
      }
    }
    this.descriptor = null;
    this.chunks.clear();
    this.ack(command, AckOperation.GraphAbort);
  }

  private handle(command: HostGraphCommand): void {
    this.commands.push(command);
    switch (command.kind) {
      case "graphStatus":
        this.ack(command, AckOperation.GraphStatus);
        this.publishInventory();
        break;
      case "graphBegin":
        this.handleBegin(command);
        break;
      case "graphChunk":
        this.handleChunk(command);
        break;
      case "graphFinish":
        this.handleFinish(command);
        break;
      case "graphAbort":
        this.handleAbort(command);
        break;
      case "graphActivate":
        this.handleActivate(command);
        break;
      case "graphRollback":
        this.handleRollback(command);
        break;
    }
  }

  async write(data: Uint8Array): Promise<void> {
    if (this.closed) {
      throw new Error("simulated transport is closed");
    }
    for (const payload of this.input.push(data)) {
      this.handle(decodeHostCommand(payload));
    }
  }

  read(_timeoutMs: number): Promise<Uint8Array | null> {
    return Promise.resolve(this.queued.shift() ?? null);
  }

  async close(): Promise<void> {
    this.closed = true;
  }
}

async function fixture(): Promise<Uint8Array> {
  return Bun.file(fixturePath).bytes();
}

describe("graph deployment", () => {
  test("stages every node before activation, survives a lost acknowledgement, reports status, and rolls back", async () => {
    const image = await fixture();
    const transport = new SimulatedNetworkTransport(image, {
      transport: { 2: { rxDropped: 3, txDropped: 4, busErrors: 5 } },
    });
    transport.dropNextAck("graphBegin");
    const previousActive = transport.activeIdentities()[0]!;
    transport.dropNextAck("graphActivate");
    const client = new PihoSerialClient(transport);

    const deployed = await deployGraph(client, image, { timeoutMs: 500 });
    expect(deployed.ok).toBe(true);
    expect(deployed.phase).toBe("complete");
    expect(deployed.activated).toBe(true);
    expect(deployed.expectedDevices).toEqual([1, 2, 7, 8]);
    expect(deployed.nodes.every((node) => node.active?.generation === deployed.identity.generation))
      .toBe(true);
    expect(transport.commands.filter((command) => command.kind === "graphBegin")).toHaveLength(2);
    expect(transport.commands.filter((command) => command.kind === "graphActivate")).toHaveLength(2);

    const status = await readNetworkStatus(client, image, { timeoutMs: 500 });
    expect(status.ok).toBe(true);
    expect(status.activeAgreement).toEqual(deployed.identity);
    expect(status.nodes.every((node) => node.complete && node.role !== null)).toBe(true);
    expect(status.issues.some((issue) => issue.kind === "transport")).toBe(true);
    expect(status.nodes.find((node) => node.id === 2)).toMatchObject({
      rxDropped: 3,
      txDropped: 4,
      busErrors: 5,
    });

    const rolledBack = await rollbackGraph(client, { timeoutMs: 500 });
    expect(rolledBack.ok).toBe(true);
    expect(rolledBack.identity).toEqual(previousActive);
    expect(transport.activeIdentities().every((active) =>
      active.generation === previousActive.generation &&
      active.checksum === previousActive.checksum)).toBe(true);
    await client.close();
  });

  test("preflight blocks missing, wrong-role, incompatible, and unhealthy nodes before transfer", async () => {
    const image = await fixture();
    const transport = new SimulatedNetworkTransport(image, {
      omit: [8],
      roles: { 7: "input" },
      formats: { 2: 2 },
      executorApis: { 1: 0 },
      storageErrors: { 1: GraphStoreError.MetadataWrite },
      extra: [{ id: 9, role: "output" }],
    });
    const client = new PihoSerialClient(transport);

    const report = await deployGraph(client, image, { timeoutMs: 200 });
    expect(report.ok).toBe(false);
    expect(report.phase).toBe("preflight");
    expect(report.issues.map((issue) => issue.kind)).toEqual(expect.arrayContaining([
      "missing",
      "wrong_role",
      "incompatible_format",
      "incompatible_executor",
      "storage",
      "unexpected",
    ]));
    expect(transport.commands.some((command) => command.kind === "graphBegin")).toBe(false);
    await client.close();
  });

  test("does not report an empty network as healthy", async () => {
    const image = await fixture();
    const transport = new SimulatedNetworkTransport(image, { omit: [1, 2, 7, 8] });
    const client = new PihoSerialClient(transport);

    const report = await readNetworkStatus(client, null, { timeoutMs: 200 });
    expect(report.ok).toBe(false);
    expect(report.discoveredDevices).toEqual([]);
    expect(report.issues).toContainEqual(expect.objectContaining({
      kind: "missing",
      detail: "no graph devices were discovered",
    }));
    await client.close();
  });

  test("reports a timed-out device by ID while retaining the active graph", async () => {
    const image = await fixture();
    const transport = new SimulatedNetworkTransport(image);
    const initialActive = transport.activeIdentities();
    transport.failure = {
      sequence: 2,
      missingDevice: 8,
      rejectedDevice: null,
      error: GraphUpdateError.None,
    };
    const client = new PihoSerialClient(transport);

    const report = await deployGraph(client, image, { timeoutMs: 300 });
    expect(report.ok).toBe(false);
    expect(report.phase).toBe("transfer");
    expect(report.missingDevices).toEqual([8]);
    expect(report.rejectedDevices).toEqual([]);
    expect(report.issues).toContainEqual(expect.objectContaining({
      kind: "timeout",
      devices: [8],
    }));
    expect(transport.activeIdentities()).toEqual(initialActive);
    await client.close();
  });

  test("identifies rejected and missing nodes without replacing active state, then retries idempotently", async () => {
    const image = await fixture();
    const transport = new SimulatedNetworkTransport(image);
    const initialActive = transport.activeIdentities();
    transport.failure = {
      sequence: 3,
      missingDevice: 8,
      rejectedDevice: 7,
      error: GraphUpdateError.Storage,
    };
    const client = new PihoSerialClient(transport);

    const failed = await deployGraph(client, image, { timeoutMs: 300 });
    expect(failed.ok).toBe(false);
    expect(failed.phase).toBe("transfer");
    expect(failed.missingDevices).toEqual([8]);
    expect(failed.rejectedDevices).toEqual([7]);
    expect(failed.issues[0]?.detail).toContain("Storage");
    expect(transport.activeIdentities()).toEqual(initialActive);

    transport.failure = null;
    transport.dropNextAck("graphChunk");
    const retried = await deployGraph(client, image, { timeoutMs: 500 });
    expect(retried.ok).toBe(true);
    expect(retried.activated).toBe(true);
    expect(transport.commands.some((command) => command.kind === "graphAbort")).toBe(true);
    expect(transport.duplicateChunks).toBeGreaterThan(0);
    await client.close();
  });
});
