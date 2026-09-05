#!/usr/bin/env bun

import { readFile, writeFile } from "node:fs/promises";
import { compileFlowDocument } from "./compile.ts";
import { FlowValidationError, GraphImageError } from "./errors.ts";
import { inspectGraphImage } from "./inspect.ts";
import { parseStrictJson } from "./json.ts";
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
    };

const USAGE = `Usage:
  piho-flow validate <flow.json> [--executor-api <version>]
  piho-flow compile <flow.json> --output <graph.phg> [--executor-api <version>]
  piho-flow inspect <graph.phg>
`;

class CliUsageError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "CliUsageError";
  }
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

function parseInvocation(arguments_: readonly string[]): Invocation | null {
  if (arguments_.length === 0 || arguments_[0] === "--help" || arguments_[0] === "-h") {
    return null;
  }
  const command = arguments_[0];
  if (command !== "validate" && command !== "compile" && command !== "inspect") {
    throw new CliUsageError(`unknown command ${JSON.stringify(command)}`);
  }
  const input = arguments_[1];
  if (input === undefined || input.startsWith("-")) {
    throw new CliUsageError(`${command} requires an input path`);
  }

  let output: string | undefined;
  let targetExecutorApi = EXECUTOR_API_VERSION;
  let targetExecutorApiSpecified = false;
  for (let index = 2; index < arguments_.length; index += 1) {
    const option = arguments_[index];
    if (option === "--executor-api" && command !== "inspect") {
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
      const value = arguments_[index + 1];
      if (value === undefined || value.startsWith("-")) {
        throw new CliUsageError("--output requires a path");
      }
      output = value;
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
  if (command === "validate") {
    return { command, input, targetExecutorApi };
  }
  return { command, input };
}

async function run(invocation: Invocation): Promise<void> {
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

async function main(): Promise<void> {
  try {
    const invocation = parseInvocation(process.argv.slice(2));
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
    if (error instanceof FlowValidationError || error instanceof GraphImageError) {
      process.stderr.write(`${error.message}\n`);
      process.exitCode = 2;
      return;
    }
    const message = error instanceof Error ? error.message : String(error);
    process.stderr.write(`error: ${message}\n`);
    process.exitCode = 1;
  }
}

await main();
