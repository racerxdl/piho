# Flow executor semantics

This document freezes the installation-independent behavior for graph format 1 and executor API 1. It defines runtime behavior, not the binary representation or authoring syntax.

All examples are synthetic. Production flow exports, names, topics, device mappings, credentials, and installation-specific topology are private and must not be committed to this repository or attached to public issues.

## Firmware and graph boundary

Firmware provides a bounded interpreter for the primitives in this document. The graph supplies devices, routes, edge selection, debounce intervals, action targets, operation parameters, and delays at runtime.

Changing those graph values must not require rebuilding, rebooting, or reflashing firmware. Adding a new primitive or changing the meaning of an existing primitive requires a new executor API and may require firmware replacement.

Graph format version and executor API version are independent. A board rejects a graph that requires an executor API it does not implement.

## Logical I/O model

Piho has 32 physical device IDs and 16 logical pins per device. Input and output polarity is normalized at the hardware boundary. Graphs operate only on logical inactive (`false`) and active (`true`) values; they never encode electrical polarity.

A format-1 source is one digital input endpoint:

- `device`: physical input device ID, `0..31`.
- `pin`: local input pin, `0..15`.
- `debounce_ms`: stable interval, `0..5000` milliseconds.
- `edge`: `rising`, `falling`, or `changed`.

Every source route is owned by the physical board named by `source.device`. Only that board evaluates the route. Every board stores the same graph, but no other board may execute that source route.

## Input and edge behavior

Input samples become logical values before debouncing. Each pin has independent candidate state and candidate timestamp.

- `rising`: stable `false` to stable `true`.
- `falling`: stable `true` to stable `false`.
- `changed`: either stable transition.

The first sample after boot establishes a baseline and emits no edge. Activating another graph also samples current inputs into a new baseline and emits no edge. Changing `debounce_ms` cannot synthesize an event from a value that was already stable before activation.

One physical transition produces one source event token. Every action emitted from that transition carries the same token.

## Routes, fan-in, and fan-out

A route connects one source and edge selector to one or more actions. One source event may emit at most 16 actions.

A rising transition matches both `rising` and `changed` routes; a falling transition matches both `falling` and `changed` routes. The 16-action limit applies to their combined output. The compiler rejects a graph that would emit the same action ID through both matching selectors, so one physical transition never invokes one action twice.

Fan-out is represented by multiple actions on one route. Fan-in is represented by multiple independently owned source routes that reference the same action. There is no implicit shared evaluator and no broadcast evaluation.

Format 1 does not contain arbitrary boolean joins, cycles, scripts, expressions, schedules, HTTP calls, MQTT calls, or cross-device mutable variables. Those remain host-side behavior. A later executor API may add explicit stateful primitives, but format-1 compilers must reject them rather than approximate them.

Duplicate action references within one route are invalid. This prevents an accidental duplicate `toggle` from cancelling or repeating an intended operation.

## Actions

Each action has a stable 16-bit ID and exactly one physical output target:

- `target_device`: output device ID, `0..31`.
- `target_pin`: local output pin, `0..15`.
- `operation`: one of the operations below.
- `delay_ms`: execution delay, `0..86400000` milliseconds.

Actions are addressed by ID on CAN. The output board resolves the ID from its own copy of the active graph and verifies that the action targets that board before changing GPIO.

### `set`

Set the logical output to the constant action value. Repeating the request has the same result and is idempotent.

### `copy_source`

Set the logical output to the debounced logical source value carried by the source event. This is valid only with a `changed` route. The compiler rejects it for edge-only events that do not carry a source value.

### `toggle`

Invert the current logical output. This operation is non-idempotent. The output board must deduplicate retries using graph generation, source device, event token, and action ID.

### `pulse`

Set the logical output active immediately, then set it inactive after `duration_ms`, where `1 <= duration_ms <= 86400000`.

A new distinct pulse for an already active pulse restarts its deadline. A retransmission with the same event token is a duplicate and does not restart it. A `set`, `copy_source`, or `toggle` action targeting the same pin cancels any pending pulse deadline before applying its value.

## Delayed execution

`delay_ms` delays dispatch at the executor/source board. The output board receives the action only when it is due. Delay scheduling is nonblocking and uses wrap-safe monotonic time comparisons.

Distinct source events create distinct delayed invocations, including events targeting the same action. The executor keeps at most 32 delayed invocations. If that queue is full, it rejects the new invocation, increments an observable overflow counter, and preserves already accepted work.

Delayed invocations are volatile across board reset. Format 1 does not write timer state to flash. The source input must transition again to create another event after reboot.

## Delivery and acknowledgement

An input/executor board keeps at most 32 action invocations awaiting application acknowledgement. A request has one immutable event token across all retries. Event tokens are nonzero 17-bit values (`1..131071`) and wrap to 1; bounded CAN throughput makes the wrap interval longer than the output deduplication window. The request preserves the full 32-bit graph generation.

`ExecuteAction` and `ActionAck` each use one classic-CAN frame with an eight-byte payload. The target or acknowledgement destination is in the extended identifier. The payload carries the generation and a packed action ID, event token, source or output device, and source value. Message types 10 and 11 are reserved for these runtime frames; graph-transfer types begin at 16, so runtime actions win CAN arbitration when their common protocol header is equal.

- CAN transmission success alone does not prove that the output was changed.
- The target returns `executed`, `already_executed`, `wrong_generation`, `unknown_action`, `wrong_target`, `invalid_action`, or `unavailable_output`.
- The sender makes at most three attempts, 100 milliseconds apart, with the same event token.
- A pending invocation expires after 300 milliseconds; a stalled board never transmits an old retry after the output deduplication window.
- An `unavailable_output` response is retryable within that lifetime; other rejection statuses are terminal.
- Exhausted or expired retries produce an error counter without blocking later input processing.

An output board retains up to 64 executed action keys for 1000 milliseconds, longer than the complete retry window. The key is graph generation, source device, event token, and action ID. Duplicate `set`, `copy_source`, `toggle`, and `pulse` requests return `already_executed` without applying the operation again. If every deduplication slot is still live, the board returns `unavailable_output` before touching GPIO rather than risk repeating a non-idempotent action.

An action request from another graph generation, with an unknown action ID, or with a target that does not match the receiving board is rejected without touching GPIO. Retry, acknowledgement timeout, rejection, exhaustion, deduplication, and capacity failures have saturating status counters.

## Graph activation

The active graph is immutable. An update is written and validated in an inactive storage slot while the active graph continues operating.

Activation occurs at one main-loop boundary:

1. Stop accepting new evaluations from the old graph.
2. Preserve already transmitted actions; cancel unsent delayed invocations from the old generation.
3. Load the new local execution section.
4. Establish current input levels as the new baseline.
5. Reset graph-local event and timer state.
6. Resume evaluation under the new generation.

Every board persists the complete image in three LittleFS graph slots. Three slots are required to keep an immutable active graph and its rollback generation while a third generation is received. Chunks are limited to 256 bytes; CRC32 is accumulated while writing, and read-back validation uses the bounded `GraphImageSource` interface rather than allocating a complete-image buffer.

Two CRC-protected metadata journal copies select the active, staged, rollback, and receiving slots by a monotonic sequence. Metadata replacement uses a verified temporary file and LittleFS atomic rename. A staged graph is never activated by reboot. Activation makes the previous active graph the rollback generation; rollback atomically swaps those two generations.

On boot, an interrupted receive is discarded without changing the active or rollback graph. An invalid staged or rollback slot is removed from metadata. If the active slot is invalid but the recorded rollback slot validates, recovery promotes that rollback deterministically; otherwise autonomous execution remains disabled. Orphan image bytes are never inferred as active state when metadata is missing or corrupt.

### Network staging protocol

The USB-connected node is the gateway for one bounded update session. `GraphBegin` identifies a nonzero transfer ID, generation, and image size. Three companion frames announce graph format/executor API, the exact 32-bit expected-device bitmap, and image CRC32. A board begins writing only after all four parts agree. A second concurrent transfer or a conflicting repeated announcement is rejected.

Image transport is stop-and-wait: each `GraphChunk` contains the transfer ID, a 16-bit sequence, and one to four bytes. The gateway advances only after every expected device reports the next sequence. An identical retry is idempotent; a conflicting duplicate, missing sequence, or out-of-order sequence is rejected. `GraphFinish` is accepted only at the exact calculated sequence count. Each board then performs independent bounded image validation and reports `staged` only when the identity and graph device bitmap match the announcement.

Each node reports status as an identity frame and a progress frame. The gateway combines only matching transfer ID, generation, and checksum observations into ready, progressed, staged, rejected, active, rollback, and missing device bitmaps. It refuses activation unless the staged bitmap equals the announced expected-device bitmap and no expected board rejected the image.

Activation and rollback carry generation plus checksum and are idempotent. The gateway retransmits them every 50 milliseconds until all expected boards report the requested identity; a board that missed an earlier broadcast therefore converges. Begin, chunk, finish, activation, rollback, and status retry state is fixed-size. Five seconds without progress rejects the session and discards only the receiving or staged candidate. Explicit abort has the same isolation property.

Graph-transfer traffic uses CAN message types 16 through 27 and a distinct low-priority software queue. Runtime action/acknowledgement frames and health traffic are always dequeued first and also have lower CAN identifiers, so deployment cannot sit ahead of runtime work.

Outputs are not reset merely because a graph activates. They change only through an explicit action or normal safe boot initialization.

## Failure behavior

- Invalid, oversized, incompatible, or corrupt graphs never replace the active graph.
- Missing target boards do not prevent local input sampling, but their actions eventually fail through bounded retry.
- Queue exhaustion is observable and never silently overwrites accepted work.
- Unknown primitives are rejected during validation.
- Integer overflow, invalid time ranges, invalid roles, invalid pins, and invalid references are validation errors.
- A board with no valid active graph performs no autonomous actions but remains available for status, manual output commands, and graph deployment.

## Format-1 limits

These are protocol and runtime limits, not dynamically allocated targets:

| Resource | Limit |
| --- | ---: |
| Complete graph image | 16 KiB |
| Devices | 32 |
| Pins per device | 16 |
| Flows | 256 |
| Routes | 512 |
| Actions | 512 |
| Local routes loaded by one board | 128 |
| Local output actions loaded by one board | 128 |
| Actions emitted by one source event | 16 |
| Delayed invocations per executor | 32 |
| Unacknowledged invocations per executor | 32 |
| Recent executed-action keys per output | 64 |
| Event token | 1..131071 |
| Action attempts | 3 at 100 ms intervals |
| Pulse timers per output board | 16 |
| Debounce interval | 0..5000 ms |
| Action delay or pulse duration | 0..86400000 ms |
| Action emissions serviced per loop | 8 |

The compiler must fail when a graph exceeds any limit. Firmware must not truncate it.

## Synthetic example

The following is conceptual and does not define the authoring syntax:

```text
device 1 input pin 2, rising, debounce 25 ms
  -> action 100: device 7 output pin 3, toggle
  -> action 101: device 8 output pin 4, set true after 60 seconds

device 2 input pin 5, changed, debounce 40 ms
  -> action 102: device 7 output pin 6, copy source state
```

The first route is evaluated only by device 1. Device 7 validates and executes action 100; device 8 validates and executes action 101 when device 1's delay expires. The second route is evaluated only by device 2.

## Explicitly unsupported in executor API 1

- User-provided source code or scripts.
- Arbitrary expressions.
- General graph cycles.
- Multi-source boolean joins.
- Wall-clock schedules.
- Persistent delayed timers.
- MQTT, HTTP, camera, notification, or Internet actions.
- Dynamic allocation or graph-defined retry loops.
- Executing a source route on every board.

These operations remain in the host control plane until a future bounded primitive is designed and versioned.