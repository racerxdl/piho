export const SERIAL_PROTOCOL_VERSION = 1;
export const SERIAL_PAYLOAD_CAPACITY = 128;
export const GRAPH_CHUNK_BYTES = 4;

export enum GraphUpdateState {
  Idle = 0,
  Receiving = 1,
  Validating = 2,
  Staged = 3,
  Active = 4,
  Rollback = 5,
  Rejected = 6,
}

export enum GraphUpdateError {
  None = 0,
  Conflict = 1,
  InvalidDescriptor = 2,
  StaleGeneration = 3,
  Storage = 4,
  OutOfOrder = 5,
  ConflictingChunk = 6,
  MissingChunk = 7,
  InvalidImage = 8,
  Timeout = 9,
  NotStaged = 10,
  NotReady = 11,
  SendFailed = 12,
  Aborted = 13,
  WrongRole = 14,
  Incompatible = 15,
}

export enum GraphStoreState {
  Empty = 0,
  Receiving = 1,
  Staged = 2,
  Active = 3,
  Invalid = 4,
  Rollback = 5,
}

export enum GraphStoreError {
  None = 0,
  Mount = 1,
  MetadataRead = 2,
  MetadataCorrupt = 3,
  MetadataWrite = 4,
  InvalidArgument = 5,
  Busy = 6,
  NoSlot = 7,
  SlotOpen = 8,
  SlotWrite = 9,
  SlotClose = 10,
  SlotRead = 11,
  InvalidLength = 12,
  InvalidChecksum = 13,
  InvalidImage = 14,
  Interrupted = 15,
  NoStagedGraph = 16,
  NoRollbackGraph = 17,
  NoActiveGraph = 18,
}

export enum GraphNodeStatusPart {
  Capabilities = 1,
  State = 2,
  Active = 3,
  Staged = 4,
  Rollback = 5,
  Manifests = 6,
  RollbackManifest = 7,
  TransportDrops = 8,
  TransportErrors = 9,
}

export enum AckOperation {
  GraphBegin = 8,
  GraphChunk = 9,
  GraphFinish = 10,
  GraphAbort = 11,
  GraphActivate = 12,
  GraphRollback = 13,
  GraphStatus = 14,
}

export type DeviceRole = "input" | "output";

export interface GraphDescriptor {
  readonly transferId: number;
  readonly generation: number;
  readonly imageSize: number;
  readonly format: number;
  readonly executorApi: number;
  readonly expectedDevices: number;
  readonly checksum: number;
}

export type HostGraphCommand =
  | { readonly kind: "graphBegin"; readonly descriptor: GraphDescriptor }
  | {
      readonly kind: "graphChunk";
      readonly transferId: number;
      readonly sequence: number;
      readonly data: Uint8Array;
    }
  | { readonly kind: "graphFinish"; readonly transferId: number; readonly sequenceCount: number }
  | { readonly kind: "graphAbort"; readonly transferId: number }
  | { readonly kind: "graphActivate" }
  | {
      readonly kind: "graphRollback";
      readonly generation: number;
      readonly checksum: number;
      readonly expectedDevices: number;
    }
  | { readonly kind: "graphStatus"; readonly transferId: number };

export interface GraphUpdateDeviceEvent {
  readonly kind: "graphUpdate";
  readonly transferId: number;
  readonly generation: number;
  readonly checksum: number;
  readonly expectedDevices: number;
  readonly readyDevices: number;
  readonly progressedDevices: number;
  readonly stagedDevices: number;
  readonly rejectedDevices: number;
  readonly activeDevices: number;
  readonly rollbackDevices: number;
  readonly missingDevices: number;
  readonly nextSequence: number;
  readonly sequenceCount: number;
  readonly state: GraphUpdateState;
  readonly error: GraphUpdateError;
  readonly chunkPending: boolean;
}

export interface GraphNodeStatusDeviceEvent {
  readonly kind: "graphNodeStatus";
  readonly device: number;
  readonly part: GraphNodeStatusPart;
  readonly format: number;
  readonly executorApi: number;
  readonly role: DeviceRole | null;
  readonly transferId: number;
  readonly updateState: GraphUpdateState;
  readonly updateError: GraphUpdateError;
  readonly rxDropped: number;
  readonly txDropped: number;
  readonly busErrors: number;
  readonly storeState: GraphStoreState;
  readonly storeError: GraphStoreError;
  readonly generation: number;
  readonly checksum: number;
  readonly activeDevices: number;
  readonly stagedDevices: number;
  readonly rollbackDevices: number;
}

export type DeviceEvent =
  | { readonly kind: "ack"; readonly operation: number; readonly accepted: boolean }
  | { readonly kind: "error"; readonly code: number }
  | GraphUpdateDeviceEvent
  | GraphNodeStatusDeviceEvent
  | { readonly kind: "other" };

function assertUint32(name: string, value: number): void {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff_ffff) {
    throw new Error(`${name} must be an unsigned 32-bit integer`);
  }
}

function concatenate(parts: readonly Uint8Array[]): Uint8Array {
  const length = parts.reduce((total, part) => total + part.length, 0);
  const output = new Uint8Array(length);
  let offset = 0;
  for (const part of parts) {
    output.set(part, offset);
    offset += part.length;
  }
  return output;
}

function encodeVarint(value: number): Uint8Array {
  assertUint32("protobuf value", value);
  const encoded: number[] = [];
  let remaining = value;
  do {
    const byte = remaining % 128;
    remaining = Math.floor(remaining / 128);
    encoded.push(byte | (remaining === 0 ? 0 : 0x80));
  } while (remaining !== 0);
  return Uint8Array.from(encoded);
}

function varintField(field: number, value: number): Uint8Array {
  if (value === 0) {
    return new Uint8Array();
  }
  return concatenate([encodeVarint(field * 8), encodeVarint(value)]);
}

function bytesField(field: number, value: Uint8Array): Uint8Array {
  return concatenate([encodeVarint(field * 8 + 2), encodeVarint(value.length), value]);
}

function messageField(field: number, value: Uint8Array): Uint8Array {
  return bytesField(field, value);
}

function encodeFields(fields: readonly Uint8Array[]): Uint8Array {
  return concatenate(fields.filter((field) => field.length !== 0));
}

export function encodeHostCommand(command: HostGraphCommand): Uint8Array {
  switch (command.kind) {
    case "graphBegin": {
      const descriptor = command.descriptor;
      const body = encodeFields([
        varintField(1, descriptor.transferId),
        varintField(2, descriptor.generation),
        varintField(3, descriptor.imageSize),
        varintField(4, descriptor.format),
        varintField(5, descriptor.executorApi),
        varintField(6, descriptor.expectedDevices),
        varintField(7, descriptor.checksum),
      ]);
      return messageField(9, body);
    }
    case "graphChunk": {
      if (command.data.length === 0 || command.data.length > GRAPH_CHUNK_BYTES) {
        throw new Error(`graph chunk must contain 1-${GRAPH_CHUNK_BYTES} bytes`);
      }
      const body = encodeFields([
        varintField(1, command.transferId),
        varintField(2, command.sequence),
        bytesField(3, command.data),
      ]);
      return messageField(10, body);
    }
    case "graphFinish":
      return messageField(
        11,
        encodeFields([
          varintField(1, command.transferId),
          varintField(2, command.sequenceCount),
        ]),
      );
    case "graphAbort":
      return messageField(12, varintField(1, command.transferId));
    case "graphActivate":
      return messageField(13, new Uint8Array());
    case "graphRollback":
      return messageField(
        14,
        encodeFields([
          varintField(1, command.generation),
          varintField(2, command.checksum),
          varintField(3, command.expectedDevices),
        ]),
      );
    case "graphStatus":
      return messageField(15, varintField(1, command.transferId));
  }
}

class ProtobufReader {
  private offset = 0;

  constructor(private readonly bytes: Uint8Array) {}

  get done(): boolean {
    return this.offset === this.bytes.length;
  }

  readVarint(): number {
    let value = 0;
    let multiplier = 1;
    for (let index = 0; index < 5; index += 1) {
      const byte = this.bytes[this.offset++];
      if (byte === undefined) {
        throw new Error("truncated protobuf varint");
      }
      if (index === 4 && byte > 0x0f) {
        throw new Error("protobuf uint32 overflow");
      }
      value += (byte & 0x7f) * multiplier;
      if ((byte & 0x80) === 0) {
        return value;
      }
      multiplier *= 128;
    }
    throw new Error("protobuf varint exceeds uint32");
  }

  readBytes(): Uint8Array {
    const length = this.readVarint();
    if (length > this.bytes.length - this.offset) {
      throw new Error("truncated protobuf field");
    }
    const value = this.bytes.slice(this.offset, this.offset + length);
    this.offset += length;
    return value;
  }

  next(): { readonly field: number; readonly wireType: number } {
    const key = this.readVarint();
    const field = Math.floor(key / 8);
    const wireType = key & 7;
    if (field === 0) {
      throw new Error("protobuf field zero is invalid");
    }
    return { field, wireType };
  }

  skip(wireType: number): void {
    if (wireType === 0) {
      this.readVarint();
      return;
    }
    if (wireType === 2) {
      this.readBytes();
      return;
    }
    throw new Error(`unsupported protobuf wire type ${wireType}`);
  }
}

function decodeVarintMessage(bytes: Uint8Array, maximumField: number): number[] {
  const values = Array.from({ length: maximumField + 1 }, () => 0);
  const reader = new ProtobufReader(bytes);
  while (!reader.done) {
    const { field, wireType } = reader.next();
    if (wireType !== 0 || field > maximumField) {
      reader.skip(wireType);
      continue;
    }
    values[field] = reader.readVarint();
  }
  return values;
}

function value(values: readonly number[], field: number): number {
  return values[field] ?? 0;
}

function decodeAck(bytes: Uint8Array): DeviceEvent {
  const fields = decodeVarintMessage(bytes, 2);
  return { kind: "ack", operation: value(fields, 1), accepted: value(fields, 2) !== 0 };
}

function decodeError(bytes: Uint8Array): DeviceEvent {
  const fields = decodeVarintMessage(bytes, 1);
  return { kind: "error", code: value(fields, 1) };
}

function decodeGraphUpdate(bytes: Uint8Array): GraphUpdateDeviceEvent {
  const fields = decodeVarintMessage(bytes, 16);
  return {
    kind: "graphUpdate",
    transferId: value(fields, 1),
    generation: value(fields, 2),
    checksum: value(fields, 3),
    expectedDevices: value(fields, 4),
    readyDevices: value(fields, 5),
    progressedDevices: value(fields, 6),
    stagedDevices: value(fields, 7),
    rejectedDevices: value(fields, 8),
    activeDevices: value(fields, 9),
    rollbackDevices: value(fields, 10),
    missingDevices: value(fields, 11),
    nextSequence: value(fields, 12),
    sequenceCount: value(fields, 13),
    state: value(fields, 14) as GraphUpdateState,
    error: value(fields, 15) as GraphUpdateError,
    chunkPending: value(fields, 16) !== 0,
  };
}

function role(code: number): DeviceRole | null {
  if (code === 1) {
    return "input";
  }
  if (code === 2) {
    return "output";
  }
  return null;
}

function decodeGraphNodeStatus(bytes: Uint8Array): GraphNodeStatusDeviceEvent {
  const fields = decodeVarintMessage(bytes, 18);
  return {
    kind: "graphNodeStatus",
    device: value(fields, 1),
    part: value(fields, 2) as GraphNodeStatusPart,
    format: value(fields, 3),
    executorApi: value(fields, 4),
    role: role(value(fields, 5)),
    transferId: value(fields, 6),
    updateState: value(fields, 7) as GraphUpdateState,
    updateError: value(fields, 8) as GraphUpdateError,
    storeState: value(fields, 9) as GraphStoreState,
    storeError: value(fields, 10) as GraphStoreError,
    generation: value(fields, 11),
    checksum: value(fields, 12),
    activeDevices: value(fields, 13),
    stagedDevices: value(fields, 14),
    rollbackDevices: value(fields, 15),
    rxDropped: value(fields, 16),
    txDropped: value(fields, 17),
    busErrors: value(fields, 18),
  };
}

export function decodeDeviceEvent(bytes: Uint8Array): DeviceEvent {
  const reader = new ProtobufReader(bytes);
  let decoded: DeviceEvent | null = null;
  while (!reader.done) {
    const { field, wireType } = reader.next();
    if (wireType !== 2) {
      reader.skip(wireType);
      continue;
    }
    const body = reader.readBytes();
    switch (field) {
      case 3:
        decoded = decodeError(body);
        break;
      case 4:
        decoded = decodeAck(body);
        break;
      case 5:
        decoded = decodeGraphUpdate(body);
        break;
      case 6:
        decoded = decodeGraphNodeStatus(body);
        break;
      default:
        decoded = { kind: "other" };
        break;
    }
  }
  return decoded ?? { kind: "other" };
}

function encodeGraphUpdate(event: GraphUpdateDeviceEvent): Uint8Array {
  return encodeFields([
    varintField(1, event.transferId),
    varintField(2, event.generation),
    varintField(3, event.checksum),
    varintField(4, event.expectedDevices),
    varintField(5, event.readyDevices),
    varintField(6, event.progressedDevices),
    varintField(7, event.stagedDevices),
    varintField(8, event.rejectedDevices),
    varintField(9, event.activeDevices),
    varintField(10, event.rollbackDevices),
    varintField(11, event.missingDevices),
    varintField(12, event.nextSequence),
    varintField(13, event.sequenceCount),
    varintField(14, event.state),
    varintField(15, event.error),
    varintField(16, event.chunkPending ? 1 : 0),
  ]);
}

function roleCode(deviceRole: DeviceRole | null): number {
  return deviceRole === "input" ? 1 : deviceRole === "output" ? 2 : 0;
}

function encodeGraphNodeStatus(event: GraphNodeStatusDeviceEvent): Uint8Array {
  return encodeFields([
    varintField(1, event.device),
    varintField(2, event.part),
    varintField(3, event.format),
    varintField(4, event.executorApi),
    varintField(5, roleCode(event.role)),
    varintField(6, event.transferId),
    varintField(7, event.updateState),
    varintField(8, event.updateError),
    varintField(9, event.storeState),
    varintField(10, event.storeError),
    varintField(11, event.generation),
    varintField(12, event.checksum),
    varintField(13, event.activeDevices),
    varintField(14, event.stagedDevices),
    varintField(15, event.rollbackDevices),
    varintField(16, event.rxDropped),
    varintField(17, event.txDropped),
    varintField(18, event.busErrors),
  ]);
}

export function encodeDeviceEvent(event: DeviceEvent): Uint8Array {
  switch (event.kind) {
    case "ack":
      return messageField(
        4,
        encodeFields([
          varintField(1, event.operation),
          varintField(2, event.accepted ? 1 : 0),
        ]),
      );
    case "error":
      return messageField(3, varintField(1, event.code));
    case "graphUpdate":
      return messageField(5, encodeGraphUpdate(event));
    case "graphNodeStatus":
      return messageField(6, encodeGraphNodeStatus(event));
    case "other":
      throw new Error("cannot encode an unspecified device event");
  }
}

export function decodeHostCommand(bytes: Uint8Array): HostGraphCommand {
  const reader = new ProtobufReader(bytes);
  let command: HostGraphCommand | null = null;
  while (!reader.done) {
    const { field, wireType } = reader.next();
    if (wireType !== 2) {
      throw new Error("host command oneof must be length-delimited");
    }
    const body = reader.readBytes();
    const fields = decodeVarintMessage(body, 7);
    switch (field) {
      case 9:
        command = {
          kind: "graphBegin",
          descriptor: {
            transferId: value(fields, 1),
            generation: value(fields, 2),
            imageSize: value(fields, 3),
            format: value(fields, 4),
            executorApi: value(fields, 5),
            expectedDevices: value(fields, 6),
            checksum: value(fields, 7),
          },
        };
        break;
      case 10: {
        const bodyReader = new ProtobufReader(body);
        let transferId = 0;
        let sequence = 0;
        let data: Uint8Array = new Uint8Array();
        while (!bodyReader.done) {
          const nested = bodyReader.next();
          if (nested.field === 1 && nested.wireType === 0) {
            transferId = bodyReader.readVarint();
          } else if (nested.field === 2 && nested.wireType === 0) {
            sequence = bodyReader.readVarint();
          } else if (nested.field === 3 && nested.wireType === 2) {
            data = bodyReader.readBytes();
          } else {
            bodyReader.skip(nested.wireType);
          }
        }
        command = { kind: "graphChunk", transferId, sequence, data };
        break;
      }
      case 11:
        command = {
          kind: "graphFinish",
          transferId: value(fields, 1),
          sequenceCount: value(fields, 2),
        };
        break;
      case 12:
        command = { kind: "graphAbort", transferId: value(fields, 1) };
        break;
      case 13:
        command = { kind: "graphActivate" };
        break;
      case 14:
        command = {
          kind: "graphRollback",
          generation: value(fields, 1),
          checksum: value(fields, 2),
          expectedDevices: value(fields, 3),
        };
        break;
      case 15:
        command = { kind: "graphStatus", transferId: value(fields, 1) };
        break;
      default:
        throw new Error(`unsupported host command field ${field}`);
    }
  }
  if (command === null) {
    throw new Error("host command is empty");
  }
  return command;
}

function crc16(bytes: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc & 0x8000) !== 0 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

export function encodeSerialFrame(payload: Uint8Array): Uint8Array {
  if (payload.length > SERIAL_PAYLOAD_CAPACITY) {
    throw new Error(`serial payload exceeds ${SERIAL_PAYLOAD_CAPACITY} bytes`);
  }
  const frame = new Uint8Array(payload.length + 7);
  frame[0] = 0x50;
  frame[1] = 0x48;
  frame[2] = SERIAL_PROTOCOL_VERSION;
  frame[3] = payload.length & 0xff;
  frame[4] = payload.length >>> 8;
  frame.set(payload, 5);
  const checksum = crc16(frame.subarray(2, 5 + payload.length));
  frame[5 + payload.length] = checksum & 0xff;
  frame[6 + payload.length] = checksum >>> 8;
  return frame;
}

export class SerialFrameDecoder {
  private buffered: Uint8Array = new Uint8Array();

  push(chunk: Uint8Array): Uint8Array[] {
    this.buffered = concatenate([this.buffered, chunk]);
    const frames: Uint8Array[] = [];
    while (this.buffered.length >= 2) {
      let magic = -1;
      for (let index = 0; index + 1 < this.buffered.length; index += 1) {
        if (this.buffered[index] === 0x50 && this.buffered[index + 1] === 0x48) {
          magic = index;
          break;
        }
      }
      if (magic < 0) {
        this.buffered = this.buffered[this.buffered.length - 1] === 0x50
          ? this.buffered.slice(-1)
          : new Uint8Array();
        break;
      }
      if (magic > 0) {
        this.buffered = this.buffered.slice(magic);
      }
      if (this.buffered.length < 5) {
        break;
      }
      if (this.buffered[2] !== SERIAL_PROTOCOL_VERSION) {
        this.buffered = this.buffered.slice(1);
        continue;
      }
      const length = (this.buffered[3] ?? 0) | ((this.buffered[4] ?? 0) << 8);
      if (length > SERIAL_PAYLOAD_CAPACITY) {
        this.buffered = this.buffered.slice(1);
        continue;
      }
      const frameLength = length + 7;
      if (this.buffered.length < frameLength) {
        break;
      }
      const expected = (this.buffered[5 + length] ?? 0) | ((this.buffered[6 + length] ?? 0) << 8);
      const actual = crc16(this.buffered.subarray(2, 5 + length));
      if (expected !== actual) {
        this.buffered = this.buffered.slice(1);
        continue;
      }
      frames.push(this.buffered.slice(5, 5 + length));
      this.buffered = this.buffered.slice(frameLength);
    }
    return frames;
  }
}
