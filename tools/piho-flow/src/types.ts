export const GRAPH_FORMAT_VERSION = 1;
export const EXECUTOR_API_VERSION = 1;

export const GRAPH_LIMITS = {
  imageBytes: 16 * 1024,
  devices: 32,
  inputs: 512,
  flows: 256,
  routes: 512,
  actions: 512,
  localRoutes: 128,
  localActions: 128,
  actionsPerEvent: 16,
  debounceMs: 5_000,
  actionTimeMs: 86_400_000,
} as const;

export type DeviceRole = "input" | "output";
export type Edge = "rising" | "falling" | "changed";
export type ActionOperation = "set" | "copy_source" | "toggle" | "pulse";

export interface DeviceDefinition {
  readonly name: string;
  readonly id: number;
  readonly role: DeviceRole;
}

export interface InputDefinition {
  readonly name: string;
  readonly device: string;
  readonly pin: number;
  readonly debounceMs: number;
}

export interface ActionDefinition {
  readonly name: string;
  readonly device: string;
  readonly pin: number;
  readonly operation: ActionOperation;
  readonly value: boolean;
  readonly delayMs: number;
  readonly durationMs: number;
}

export interface FlowTriggerDefinition {
  readonly input: string;
  readonly edge: Edge;
}

export interface FlowDefinition {
  readonly name: string;
  readonly when: readonly FlowTriggerDefinition[];
  readonly actions: readonly string[];
}

export interface ValidatedFlowDocument {
  readonly format: number;
  readonly executorApi: number;
  readonly generation: number;
  readonly devices: readonly DeviceDefinition[];
  readonly inputs: readonly InputDefinition[];
  readonly actions: readonly ActionDefinition[];
  readonly flows: readonly FlowDefinition[];
}

export interface CompiledDevice {
  readonly id: number;
  readonly role: DeviceRole;
  readonly inputStart: number;
  readonly inputCount: number;
  readonly routeStart: number;
  readonly routeCount: number;
  readonly actionStart: number;
  readonly actionCount: number;
}

export interface CompiledInput {
  readonly id: number;
  readonly device: number;
  readonly pin: number;
  readonly debounceMs: number;
}

export interface CompiledRoute {
  readonly id: number;
  readonly flowId: number;
  readonly inputId: number;
  readonly ownerDevice: number;
  readonly edge: Edge;
  readonly actionIds: readonly number[];
}

export interface CompiledAction {
  readonly id: number;
  readonly targetDevice: number;
  readonly targetPin: number;
  readonly operation: ActionOperation;
  readonly value: boolean;
  readonly delayMs: number;
  readonly durationMs: number;
}

export interface CompiledGraph {
  readonly format: number;
  readonly executorApi: number;
  readonly generation: number;
  readonly flowCount: number;
  readonly devices: readonly CompiledDevice[];
  readonly inputs: readonly CompiledInput[];
  readonly routes: readonly CompiledRoute[];
  readonly actions: readonly CompiledAction[];
}
