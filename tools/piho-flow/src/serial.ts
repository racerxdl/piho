import { constants } from "node:fs";
import { open, type FileHandle } from "node:fs/promises";
import {
  decodeDeviceEvent,
  encodeHostCommand,
  encodeSerialFrame,
  SerialFrameDecoder,
  type DeviceEvent,
  type HostGraphCommand,
} from "./protocol.ts";

export interface ByteTransport {
  write(data: Uint8Array): Promise<void>;
  read(timeoutMs: number): Promise<Uint8Array | null>;
  close(): Promise<void>;
}

interface ReadWaiter {
  readonly resolve: (value: Uint8Array | null) => void;
  readonly timer: ReturnType<typeof setTimeout>;
}

export class PosixSerialTransport implements ByteTransport {
  private readonly queued: Uint8Array[] = [];
  private waiter: ReadWaiter | null = null;
  private readFailure: Error | null = null;
  private closed = false;
  private readonly readTask: Promise<void>;

  private constructor(private readonly handle: FileHandle) {
    this.readTask = this.readLoop();
  }

  static async open(path: string): Promise<PosixSerialTransport> {
    const handle = await open(path, constants.O_RDWR | constants.O_NOCTTY);
    const configure = Bun.spawn({
      cmd: ["stty", "-F", path, "115200", "raw", "-echo", "min", "0", "time", "1"],
      stdout: "ignore",
      stderr: "pipe",
    });
    const stderr = await new Response(configure.stderr).text();
    const exitCode = await configure.exited;
    if (exitCode !== 0) {
      await handle.close();
      throw new Error(`cannot configure serial port ${path}: ${stderr.trim() || `stty exited ${exitCode}`}`);
    }
    return new PosixSerialTransport(handle);
  }

  private deliver(chunk: Uint8Array): void {
    const waiter = this.waiter;
    if (waiter === null) {
      this.queued.push(chunk);
      return;
    }
    this.waiter = null;
    clearTimeout(waiter.timer);
    waiter.resolve(chunk);
  }

  private async readLoop(): Promise<void> {
    const buffer = new Uint8Array(512);
    try {
      while (!this.closed) {
        const { bytesRead } = await this.handle.read(buffer, 0, buffer.length, null);
        if (bytesRead === 0) {
          continue;
        }
        this.deliver(buffer.slice(0, bytesRead));
      }
    } catch (error) {
      if (!this.closed) {
        this.readFailure = error instanceof Error ? error : new Error(String(error));
        const waiter = this.waiter;
        if (waiter !== null) {
          this.waiter = null;
          clearTimeout(waiter.timer);
          waiter.resolve(null);
        }
      }
    }
  }

  async write(data: Uint8Array): Promise<void> {
    if (this.closed) {
      throw new Error("serial port is closed");
    }
    let offset = 0;
    while (offset < data.length) {
      const { bytesWritten } = await this.handle.write(data, offset, data.length - offset, null);
      if (bytesWritten === 0) {
        throw new Error("serial write made no progress");
      }
      offset += bytesWritten;
    }
  }

  read(timeoutMs: number): Promise<Uint8Array | null> {
    if (!Number.isInteger(timeoutMs) || timeoutMs <= 0) {
      return Promise.resolve(null);
    }
    const queued = this.queued.shift();
    if (queued !== undefined) {
      return Promise.resolve(queued);
    }
    if (this.readFailure !== null) {
      return Promise.reject(this.readFailure);
    }
    if (this.closed) {
      return Promise.resolve(null);
    }
    if (this.waiter !== null) {
      return Promise.reject(new Error("serial transport supports one pending read"));
    }
    const { promise, resolve } = Promise.withResolvers<Uint8Array | null>();
    const timer = setTimeout(() => {
      if (this.waiter?.timer === timer) {
        this.waiter = null;
        resolve(null);
      }
    }, timeoutMs);
    this.waiter = { resolve, timer };
    return promise;
  }

  async close(): Promise<void> {
    if (this.closed) {
      return;
    }
    this.closed = true;
    const waiter = this.waiter;
    if (waiter !== null) {
      this.waiter = null;
      clearTimeout(waiter.timer);
      waiter.resolve(null);
    }
    await this.handle.close();
    await this.readTask;
  }
}

export class PihoSerialClient {
  private readonly decoder = new SerialFrameDecoder();
  private readonly queuedEvents: DeviceEvent[] = [];

  constructor(private readonly transport: ByteTransport) {}

  async send(command: HostGraphCommand): Promise<void> {
    await this.transport.write(encodeSerialFrame(encodeHostCommand(command)));
  }

  async nextEvent(timeoutMs: number): Promise<DeviceEvent | null> {
    const queued = this.queuedEvents.shift();
    if (queued !== undefined) {
      return queued;
    }
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const chunk = await this.transport.read(Math.max(1, deadline - Date.now()));
      if (chunk === null) {
        return null;
      }
      for (const payload of this.decoder.push(chunk)) {
        this.queuedEvents.push(decodeDeviceEvent(payload));
      }
      const event = this.queuedEvents.shift();
      if (event !== undefined) {
        return event;
      }
    }
    return null;
  }

  close(): Promise<void> {
    return this.transport.close();
  }
}
