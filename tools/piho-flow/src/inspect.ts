import {
  GRAPH_ACTION_RECORD_SIZE,
  GRAPH_ACTION_REFERENCE_SIZE,
  GRAPH_CHECKSUM_OFFSET,
  GRAPH_DEVICE_RECORD_SIZE,
  GRAPH_HEADER_SIZE,
  GRAPH_INPUT_RECORD_SIZE,
  GRAPH_ROUTE_RECORD_SIZE,
  decodeGraphImage,
} from "./image.ts";
import type { DeviceRole } from "./types.ts";

export interface GraphSectionReport {
  readonly offset: number;
  readonly bytes: number;
  readonly records: number;
}

export interface DeviceManifestEntry {
  readonly id: number;
  readonly role: DeviceRole;
  readonly inputs: { readonly start: number; readonly count: number };
  readonly routes: { readonly start: number; readonly count: number };
  readonly actions: { readonly start: number; readonly count: number };
}

export interface GraphInspectionReport {
  readonly magic: "PHGF";
  readonly format: number;
  readonly executorApi: number;
  readonly generation: number;
  readonly imageBytes: number;
  readonly crc32: string;
  readonly counts: {
    readonly devices: number;
    readonly inputs: number;
    readonly flows: number;
    readonly routes: number;
    readonly actionReferences: number;
    readonly actions: number;
  };
  readonly sections: {
    readonly header: GraphSectionReport;
    readonly devices: GraphSectionReport;
    readonly inputs: GraphSectionReport;
    readonly routes: GraphSectionReport;
    readonly actionReferences: GraphSectionReport;
    readonly actions: GraphSectionReport;
  };
  readonly devices: readonly DeviceManifestEntry[];
}

export function inspectGraphImage(bytes: Uint8Array): GraphInspectionReport {
  const graph = decodeGraphImage(bytes);
  const actionReferenceCount = graph.routes.reduce(
    (total, route) => total + route.actionIds.length,
    0,
  );
  const deviceOffset = GRAPH_HEADER_SIZE;
  const inputOffset = deviceOffset + graph.devices.length * GRAPH_DEVICE_RECORD_SIZE;
  const routeOffset = inputOffset + graph.inputs.length * GRAPH_INPUT_RECORD_SIZE;
  const actionReferenceOffset = routeOffset + graph.routes.length * GRAPH_ROUTE_RECORD_SIZE;
  const actionOffset = actionReferenceOffset + actionReferenceCount * GRAPH_ACTION_REFERENCE_SIZE;
  const checksum = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(
    GRAPH_CHECKSUM_OFFSET,
    true,
  );

  return {
    magic: "PHGF",
    format: graph.format,
    executorApi: graph.executorApi,
    generation: graph.generation,
    imageBytes: bytes.length,
    crc32: `0x${checksum.toString(16).padStart(8, "0")}`,
    counts: {
      devices: graph.devices.length,
      inputs: graph.inputs.length,
      flows: graph.flowCount,
      routes: graph.routes.length,
      actionReferences: actionReferenceCount,
      actions: graph.actions.length,
    },
    sections: {
      header: { offset: 0, bytes: GRAPH_HEADER_SIZE, records: 1 },
      devices: {
        offset: deviceOffset,
        bytes: graph.devices.length * GRAPH_DEVICE_RECORD_SIZE,
        records: graph.devices.length,
      },
      inputs: {
        offset: inputOffset,
        bytes: graph.inputs.length * GRAPH_INPUT_RECORD_SIZE,
        records: graph.inputs.length,
      },
      routes: {
        offset: routeOffset,
        bytes: graph.routes.length * GRAPH_ROUTE_RECORD_SIZE,
        records: graph.routes.length,
      },
      actionReferences: {
        offset: actionReferenceOffset,
        bytes: actionReferenceCount * GRAPH_ACTION_REFERENCE_SIZE,
        records: actionReferenceCount,
      },
      actions: {
        offset: actionOffset,
        bytes: graph.actions.length * GRAPH_ACTION_RECORD_SIZE,
        records: graph.actions.length,
      },
    },
    devices: graph.devices.map((device) => ({
      id: device.id,
      role: device.role,
      inputs: { start: device.inputStart, count: device.inputCount },
      routes: { start: device.routeStart, count: device.routeCount },
      actions: { start: device.actionStart, count: device.actionCount },
    })),
  };
}
