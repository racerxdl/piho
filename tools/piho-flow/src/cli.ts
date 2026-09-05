#!/usr/bin/env bun

import { readFile, writeFile } from "node:fs/promises";
import { compileFlowDocument } from "./compile.ts";
import {
  deployGraph,
  readNetworkStatus,
  rollbackGraph,
  type DeploymentProgress,
  type DeploymentReport,
  type GraphIdentityReport,
  type NetworkNodeReport,
  type NetworkStatusReport,
  type RollbackReport,
} from "./deploy.ts";
import { FlowValidationError, GraphImageError } from "./errors.ts";
import { inspectGraphImage } from "./inspect.ts";
import { parseStrictJson } from "./json.ts";
import { GraphStoreError, GraphUpdateError, GraphUpdateState } from "./protocol.ts";
import { PihoSerialClient, PosixSerialTransport } from "./serial.ts";
import { EXECUTOR_API_VERSION } from "./types.ts";

type Invocation =
  | {
      readonly command: "validate";
      readonly input: string;
      readonly targetExecutorApi: number;
    }
  | {
      readonly command: "compile";
      readonly input: string;
      readonly output: string;
      readonly targetExecutorApi: number;
    }
  | {
      readonly command: "inspect";
      readonly input: string;
    }
  | {
      readonly command: "deploy";
      readonly input: string;
      readonly port: string;
      readonly timeoutMs: number;
      readonly json: boolean;
    }
  | {
      readonly command: "status";
      readonly input: string | null;
      readonly port: string;
      readonly timeoutMs: number;
      readonly json: boolean;
    }
  | {
      readonly command: "rollback";
      readonly port: string;
      readonly timeoutMs: number;
      readonly json: boolean;
    };

type SerialInvocation = Extract<Invocation, { readonly command: "deploy" | "status" | "rollback" }>;
type SerialReport = DeploymentReport | NetworkStatusReport | RollbackReport;

const DEFAULT_TIMEOUT_MS = 5_000;
const USAGE = `Usage:
  piho-flow validate <flow.json> [--executor-api <version>]
  piho-flow compile <flow.json> --output <graph.phg> [--executor-api <version>]
  piho-flow inspect <graph.phg>
  piho-flow deploy <graph.phg> --port <device> [--timeout-ms <milliseconds>] [--json]
  piho-flow status [graph.phg] --port <device> [--timeout-ms <milliseconds>] [--json]
  piho-flow rollback --port <device> [--timeout-ms <milliseconds>] [--json]
`;

class CliUsageError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CliUsageError";
  }
}

function requiredOptionValue(arguments_: readonly string[], index: number, option: string): string {
  const value = arguments_[index + 1];
  if (value === undefined || value.startsWith("-")) {
    throw new CliUsageError(`${option} requires a value`);
  }
  return value;
}

function parseExecutorApi(text: string | undefined): number {
  if (text === undefined || !/^[1-9][0-9]*$/.test(text)) {
    throw new CliUsageError("--executor-api requires a positive integer");
  }
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value > 0xffff) {
    throw new CliUsageError("--executor-api must be between 1 and 65535");
  }
  return value;
}

function parseTimeout(text: string): number {
  if (!/^[1-9][0-9]*$/.test(text)) {
    throw new CliUsageError("--timeout-ms requires an integer between 100 and 300000");
  }
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value < 100 || value > 300_000) {
    throw new CliUsageError("--timeout-ms must be between 100 and 300000");
  }
  return value;
}

function parseOfflineInvocation(command: string, arguments_: readonly string[]): Invocation {
  const input = arguments_[1];
  if (input === undefined || input.startsWith("-")) {
    throw new CliUsageError(`${command} requires an input path`);
  }
  if (command === "inspect") {
    if (arguments_.length !== 2) {
      throw new CliUsageError(`unsupported argument ${JSON.stringify(arguments_[2])}`);
    }
    return { command, input };
  }

  let output: string | undefined;
  let targetExecutorApi = EXECUTOR_API_VERSION;
  let targetExecutorApiSpecified = false;
  for (let index = 2; index < arguments_.length; index += 1) {
    const option = arguments_[index];
    if (option === "--executor-api") {
      if (targetExecutorApiSpecified) {
        throw new CliUsageError("--executor-api may be specified only once");
      }
      targetExecutorApi = parseExecutorApi(arguments_[index + 1]);
      targetExecutorApiSpecified = true;
      index += 1;
      continue;
    }
    if (option === "--output" && command === "compile") {
      if (output !== undefined) {
        throw new CliUsageError("--output may be specified only once");
      }
      output = requiredOptionValue(arguments_, index, "--output");
      index += 1;
      continue;
    }
    throw new CliUsageError(`unsupported argument ${JSON.stringify(option)}`);
  }

  if (command === "compile") {
    if (output === undefined) {
      throw new CliUsageError("compile requires --output <graph.phg>");
    }
    return { command, input, output, targetExecutorApi };
  }
  return { command: "validate", input, targetExecutorApi };
}

function parseSerialInvocation(command: "deploy" | "status" | "rollback", arguments_: readonly string[]): SerialInvocation {
  let index = 1;
  let input: string | null = null;
  if (command === "deploy") {
    const candidate = arguments_[index];
    if (candidate === undefined || candidate.startsWith("-")) {
      throw new CliUsageError("deploy requires a graph image path");
    }
    input = candidate;
    index += 1;
  } else if (command === "status" && arguments_[index] !== undefined &&
             !arguments_[index]!.startsWith("-")) {
    input = arguments_[index]!;
    index += 1;
  }

  let port: string | undefined;
  let timeoutMs = DEFAULT_TIMEOUT_MS;
  let timeoutSpecified = false;
  let json = false;
  for (; index < arguments_.length; index += 1) {
    const option = arguments_[index];
    if (option === "--port") {
      if (port !== undefined) {
        throw new CliUsageError("--port may be specified only once");
      }
      port = requiredOptionValue(arguments_, index, "--port");
      index += 1;
      continue;
    }
    if (option === "--timeout-ms") {
      if (timeoutSpecified) {
        throw new CliUsageError("--timeout-ms may be specified only once");
      }
      timeoutMs = parseTimeout(requiredOptionValue(arguments_, index, "--timeout-ms"));
      timeoutSpecified = true;
      index += 1;
      continue;
    }
    if (option === "--json") {
      if (json) {
        throw new CliUsageError("--json may be specified only once");
      }
      json = true;
      continue;
    }
    throw new CliUsageError(`unsupported argument ${JSON.stringify(option)}`);
  }
  if (port === undefined) {
    throw new CliUsageError(`${command} requires --port <device>`);
  }
  if (command === "deploy") {
    return { command, input: input!, port, timeoutMs, json };
  }
  if (command === "status") {
    return { command, input, port, timeoutMs, json };
  }
  return { command, port, timeoutMs, json };
}

function parseInvocation(arguments_: readonly string[]): Invocation | null {
  if (arguments_.length === 0 || arguments_[0] === "--help" || arguments_[0] === "-h") {
    return null;
  }
  const command = arguments_[0];
  if (command === "validate" || command === "compile" || command === "inspect") {
    return parseOfflineInvocation(command, arguments_);
  }
  if (command === "deploy" || command === "status" || command === "rollback") {
    return parseSerialInvocation(command, arguments_);
  }
  throw new CliUsageError(`unknown command ${JSON.stringify(command)}`);
}

function identityText(identity: GraphIdentityReport | null): string {
  return identity === null
    ? "none"
    : `${identity.generation}/0x${identity.checksum.toString(16).padStart(8, "0")}`;
}

function nodeText(node: NetworkNodeReport): string {
  const updateState = GraphUpdateState[node.updateState] ?? node.updateState;
  const updateError = GraphUpdateError[node.updateError] ?? node.updateError;
  const storeError = GraphStoreError[node.storeError] ?? node.storeError;
  return [
    `device ${node.id}`,
    `role=${node.role ?? "unknown"}`,
    `format=${node.format ?? "unknown"}`,
    `executor_api=${node.executorApi ?? "unknown"}`,
    `active=${identityText(node.active)}`,
    `staged=${identityText(node.staged)}`,
    `rollback=${identityText(node.rollback)}`,
    `runtime=${identityText(node.runtime)}`,
    `update=${updateState}/${updateError}`,
    `storage=${node.storeState}/${storeError}`,
    `transport=${node.rxDropped}/${node.txDropped}/${node.busErrors}`,
    `flow=${node.flowAcceptedEvents}/${node.flowEvaluatedActions}`,
    `actions=${node.actionRetries}/${node.actionRejections}`,
    `executor=${node.executorExecutedActions}/${node.executorRejectedActions}`,
    `complete=${node.complete}`,
  ].join(" ");
}

function writeHumanReport(report: SerialReport): void {
  if (report.operation === "deploy") {
    process.stdout.write(
      `deploy ${report.ok ? "succeeded" : "failed"}: phase=${report.phase} ` +
      `graph=${identityText(report.identity)} chunks=${report.completedChunks}/${report.totalChunks} ` +
      `activated=${report.activated}\n`,
    );
  } else if (report.operation === "status") {
    process.stdout.write(
      `status ${report.ok ? "healthy" : "failed"}: expected=${report.expectedDevices.join(",") || "none"} ` +
      `discovered=${report.discoveredDevices.join(",") || "none"} active=${identityText(report.activeAgreement)}\n`,
    );
  } else {
    process.stdout.write(
      `rollback ${report.ok ? "succeeded" : "failed"}: target=${identityText(report.identity)}\n`,
    );
  }
  for (const node of report.nodes) {
    process.stdout.write(`${nodeText(node)}\n`);
  }
  for (const issue of report.issues) {
    process.stdout.write(`${issue.severity}: ${issue.detail}\n`);
  }
}

function progressText(progress: DeploymentProgress): string {
  return progress.phase === "transfer"
    ? `deploy: transfer ${progress.completedChunks}/${progress.totalChunks}\n`
    : `deploy: ${progress.phase}\n`;
}

async function runSerial(invocation: SerialInvocation): Promise<void> {
  let image: Uint8Array | null = null;
  if (invocation.command === "deploy") {
    image = await readFile(invocation.input);
  } else if (invocation.command === "status" && invocation.input !== null) {
    image = await readFile(invocation.input);
  }
  const transport = await PosixSerialTransport.open(invocation.port);
  const client = new PihoSerialClient(transport);
  let report: SerialReport;
  try {
    if (invocation.command === "deploy") {
      const progress = invocation.json
        ? {}
        : {
            onProgress: (state: DeploymentProgress): void => {
              process.stderr.write(progressText(state));
            },
          };
      report = await deployGraph(client, image!, {
        timeoutMs: invocation.timeoutMs,
        ...progress,
      });
    } else if (invocation.command === "status") {
      report = await readNetworkStatus(client, image, { timeoutMs: invocation.timeoutMs });
    } else {
      report = await rollbackGraph(client, { timeoutMs: invocation.timeoutMs });
    }
  } finally {
    await client.close();
  }

  if (invocation.json) {
    process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
  } else {
    writeHumanReport(report);
  }
  if (!report.ok) {
    process.exitCode = 1;
  }
}

async function run(invocation: Invocation): Promise<void> {
  if (invocation.command === "deploy" || invocation.command === "status" ||
      invocation.command === "rollback") {
    await runSerial(invocation);
    return;
  }
  if (invocation.command === "inspect") {
    const report = inspectGraphImage(await readFile(invocation.input));
    process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
    return;
  }

  const sourceBytes = await readFile(invocation.input);
  let sourceText: string;
  try {
    sourceText = new TextDecoder("utf-8", { fatal: true }).decode(sourceBytes);
  } catch {
    throw new FlowValidationError("$", "source must be valid UTF-8");
  }
  const source = parseStrictJson(sourceText);
  if (invocation.command === "validate") {
    const { graph } = compileFlowDocument(source, invocation.targetExecutorApi);
    process.stdout.write(
      `${[
        `valid graph format ${graph.format}`,
        `executor API ${graph.executorApi}`,
        `generation ${graph.generation}`,
        `${graph.devices.length} devices`,
        `${graph.flowCount} flows`,
      ].join(", ")}\n`,
    );
    return;
  }

  const { graph, image } = compileFlowDocument(source, invocation.targetExecutorApi);
  await writeFile(invocation.output, image);
  process.stdout.write(
    `wrote ${image.length} bytes to ${invocation.output} (generation ${graph.generation})\n`,
  );
}

function isSerialInvocation(invocation: Invocation): invocation is SerialInvocation {
  return invocation.command === "deploy" || invocation.command === "status" ||
    invocation.command === "rollback";
}

async function main(): Promise<void> {
  let invocation: Invocation | null = null;
  try {
    invocation = parseInvocation(process.argv.slice(2));
    if (invocation === null) {
      process.stdout.write(USAGE);
      return;
    }
    await run(invocation);
  } catch (error) {
    if (error instanceof CliUsageError) {
      process.stderr.write(`${error.message}\n\n${USAGE}`);
      process.exitCode = 2;
      return;
    }
    const message = error instanceof Error ? error.message : String(error);
    if (invocation !== null && isSerialInvocation(invocation) && invocation.json) {
      process.stdout.write(`${JSON.stringify({
        operation: invocation.command,
        ok: false,
        error: { type: error instanceof Error ? error.name : "Error", message },
      }, null, 2)}\n`);
      process.exitCode = error instanceof FlowValidationError || error instanceof GraphImageError ? 2 : 1;
      return;
    }
    if (error instanceof FlowValidationError || error instanceof GraphImageError) {
      process.stderr.write(`${error.message}\n`);
      process.exitCode = 2;
      return;
    }
    process.stderr.write(`error: ${message}\n`);
    process.exitCode = 1;
  }
}

await main();
