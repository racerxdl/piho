# Piho

RP2040 home-automation input and output nodes connected by CAN bus.

Piho builds two Raspberry Pi Pico firmware variants:

- `input_device`: debounces configured active-low GPIO inputs, evaluates its local graph routes, and sends addressed action requests.
- `output_device`: executes addressed graph actions on 16 active-low GPIO outputs and services pulse timers without blocking the firmware loop.

## Compatibility

The current firmware uses CAN protocol version 2. It is a clean wire-protocol cutover and is not compatible with earlier Piho firmware, version-1 CAN frames, legacy trigger commands, or the old unframed USB commands. Reflash every node on a CAN network together and update the host from [`protocol/shift.proto`](protocol/shift.proto).

## Hardware mapping

| Function | GPIO |
| --- | --- |
| Inputs or outputs | 0-15 |
| CAN TX | 16 |
| CAN RX | 17 |
| Activity LED | 18 |
| Health LED | 19 |
| Error LED | 20 |
| Address bits 0-4 | 21, 22, 26, 27, 28 |

The five pulled-up address pins form a device ID from 0 through 31. Input/output polarity, CAN bitrate, and pin assignments are defined in [`include/config.h`](include/config.h). Each input's debounce duration comes from the active graph.

## Build and test

Requirements: Git with submodule support and PlatformIO Core.

```sh
git submodule update --init --recursive
pio run -e input_device
pio run -e output_device
pio test -e native
```

The PlatformIO platform, Nanopb, native test platform, and `can2040` submodule are pinned. Nanopb generates `shift.pb.c` and `shift.pb.h` into `.pio/build`; generated files are not source-controlled.

Both firmware environments enforce a maximum of 65,536 bytes of RAM and 262,144 bytes of flash through `platformio.ini`; a build fails if either budget is exceeded.

Firmware images are written to:

- `.pio/build/input_device/firmware.uf2`
- `.pio/build/output_device/firmware.uf2`

## Flow graph compiler

The Bun/TypeScript CLI in [`tools/piho-flow`](tools/piho-flow) validates strict JSON flow sources and produces deterministic, CRC-protected `.phg` graph images without connecting to hardware:

```sh
cd tools/piho-flow
bun install --frozen-lockfile
bun run typecheck
bun test
bun run flow validate examples/synthetic.json
bun run flow compile examples/synthetic.json --output /tmp/synthetic.phg
bun run flow inspect /tmp/synthetic.phg
bun run flow deploy /tmp/synthetic.phg --port /dev/ttyACM0
bun run flow status /tmp/synthetic.phg --port /dev/ttyACM0
bun run flow rollback --port /dev/ttyACM0
```

See [`docs/flow-executor-semantics.md`](docs/flow-executor-semantics.md) for runtime behavior and [`docs/flow-image-format.md`](docs/flow-image-format.md) for the authoring schema, canonical ID assignment, and binary record layout.

## CAN protocol

Piho accepts only 29-bit extended data frames in its namespace:

```text
(0x150 << 20) | (version << 16) | (message_type << 8) | address
```

Version is `2`. Ordinary addressed commands use a physical device ID (`0..31`). Health check, reset, and graph-update control frames may use broadcast `0xFF`; graph status frames carry the reporting physical device. For `ActionAck`, the low five address bits select the source board and the high three bits carry the acknowledgement status. Every message has an exact payload length:

| Type | Payload |
| --- | --- |
| Health check | empty |
| Reset | empty |
| Input state | `uint16` state, little-endian |
| Output state | `uint16` state, little-endian |
| Set pin | local pin, boolean value |
| Set byte | local byte, value |
| Execute action | 32-bit generation plus packed action ID, event token, source device, and source value |
| Action acknowledgement | 32-bit generation plus packed action ID, event token, and output device; status is in the identifier |
| Graph begin | transfer ID, generation, and image size |
| Graph compatibility | transfer ID, graph format, executor API, reserved zero |
| Graph devices | transfer ID, expected-device bitmap, reserved zero |
| Graph checksum | transfer ID and image CRC32, reserved zero |
| Graph chunk | transfer ID, sequence, and 1-4 image bytes |
| Graph finish | transfer ID and total sequence count |
| Graph abort/status request | transfer ID |
| Graph activate/rollback | generation and image CRC32 |
| Graph status identity | transfer ID, generation, state, and error |
| Graph status progress | transfer ID, image CRC32, and next sequence |
| Graph node capabilities/state | graph format, executor API, role, transfer/store state, and typed errors |
| Graph active/staged/rollback identity | generation and image CRC32 |
| Graph active/staged/rollback manifest | expected-device bitmaps |
| Graph transport counters | dropped RX/TX frames and CAN bus errors |
| Graph runtime identity | loaded generation and image CRC32 |
| Graph flow counters | accepted source events and evaluated actions |
| Graph action counters | transmission retries and remote rejections |
| Graph executor counters | executed and rejected output actions |

Frames with another namespace, remote-frame flag, unsupported type, invalid device, incorrect length, or invalid payload are rejected before dispatch.

Runtime action frames use types 10 and 11. Graph-transfer and inventory frames use types 16 through 40 and a separate low-priority transmit queue, so actions and health traffic preempt an update even when update frames are already buffered.

Global host addresses are mapped as follows:

```text
global_pin  = device * 16 + local_pin   # 0..511
global_byte = device * 2  + local_byte  # 0..63
```

## USB serial protocol

USB serial carries length-delimited Nanopb messages, not diagnostic text:

```text
"PH" | version:u8 | payload_length:u16-le | protobuf_payload | crc16:u16-le
```

The version is `1`, the payload limit is 128 bytes, and CRC-16/CCITT uses polynomial `0x1021` with initial value `0xFFFF`. The checksum covers version, encoded length, and payload. Host-to-device payloads are `HostCommand`; responses and asynchronous updates are `DeviceEvent`. Graph commands expose begin, chunk, finish, abort, activate, rollback, and status operations. `GraphUpdateEvent` reports transfer identity, progress, per-state device bitmaps, and a typed state/error. `GraphNodeStatusEvent` reports each board's role, supported graph/executor versions, stored and loaded runtime identities, manifests, storage/update state, transport counters, source-flow activity, action retries/rejections, and output-executor activity. See [`protocol/shift.proto`](protocol/shift.proto) for the command and event schema.

The parser consumes a bounded number of bytes per firmware loop, validates framing before protobuf decoding, and resynchronizes after malformed input.

## Graph deployment

Any USB-connected node can coordinate one network update session. The host starts a transfer with format, executor API, generation, image length, CRC32, and the exact bitmap of graph devices; then sends ordered four-byte chunks. Each chunk is stop-and-wait and idempotent. A duplicate with identical bytes is accepted, while missing, out-of-order, or conflicting data is rejected without modifying the active graph.

Finish succeeds only after every expected board has acknowledged all chunks and independently validated the stored image. Activation is gated on every expected board reporting the same staged generation and checksum. Each board loads and validates its role-specific runtime section before reporting activation; a runtime load failure restores the prior stored and running graph. Activate and rollback commands are retried until all expected boards report both the target stored identity and the same loaded runtime identity, so a board that misses the first broadcast converges. A five-second inactive transfer times out and discards only the receiving or staged candidate; explicit abort has the same active-graph safety property.

`piho-flow deploy` queries every connected board before transferring bytes. Preflight rejects missing or unexpected IDs, role mismatches, unsupported graph/executor versions, incomplete inventory, runtime/store identity divergence, and unrecoverable storage errors. Transfer commands have bounded retries and deadlines; all boards must report the same staged identity before activation. A failed transfer reports its phase plus missing and rejecting device IDs while preserving the prior active generation.

`piho-flow status` discovers the network without an image, or verifies IDs, roles, and stored/running generation against an optional `.phg` image. `piho-flow rollback` first requires every active board to agree on the retained rollback identity and device manifest. All three commands accept `--timeout-ms` and `--json`; JSON mode writes one machine-readable report and uses a nonzero exit status when `ok` is false. Human mode lists per-node identities, update/storage errors, transport counters, source-flow counters, action retries/rejections, and output-executor counters. Serial ports are configured as 115200-baud raw devices with `stty`, so the invoking Linux user needs read/write permission on the selected device.

## Runtime graph execution and storage

Every board stores the same versioned, CRC32-protected graph image in alternating LittleFS slots with read-back verification and rollback. At activation, an input board loads only its owned inputs, routes, and referenced actions; an output board loads only its addressed actions. The stored active generation and the loaded runtime generation are reported independently.

Input boards establish the current GPIO state as an activation baseline, so boot or graph replacement cannot synthesize an edge. Each graph input has its own debounce duration. A stable transition produces exact `current`, `changed`, `rising`, and `falling` masks; only the source input board evaluates matching routes. Immediate and delayed invocations enter the reliable action sender, which retries until the addressed output board acknowledges execution or rejection.

Output boards do not evaluate input-state triggers. They validate the graph generation, target, action ID, and event token, execute each accepted action once, and acknowledge duplicates without repeating the output change. Pulse completion uses per-pin deadlines serviced from the main loop; it never blocks CAN, USB, input sampling, or another output action.

## License

Project-authored source is licensed under Apache-2.0; see [`LICENSE`](LICENSE).

The current firmware also links the `lib/can2040` submodule, which is licensed under GPLv3. Distributed firmware binaries therefore require compliance with the GPLv3 terms applicable to that linked work. The transport is isolated behind `CanTransport` to permit a future replacement, but `can2040` remains in use for hardware compatibility. See [`lib/can2040/COPYING`](lib/can2040/COPYING) and [can2040 issue #5](https://github.com/KevinOConnor/can2040/issues/5).
