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

An input/executor board keeps at most 32 action invocations awaiting application acknowledgement. A request has one immutable event token across all retries.

- CAN transmission success alone does not prove that the output was changed.
- The target returns `executed`, `already_executed`, or a rejection reason.
- Timeouts retry with the same event token.
- Retry count and timeout are fixed firmware capabilities reported to the host, not graph-controlled unbounded values.
- Exhausted retries produce an error event and counter without blocking later input processing.

An output board keeps a bounded recent-event cache sufficient for the retry window. Duplicate `toggle` and `pulse` requests return `already_executed` without applying the operation again.

An action request from another graph generation, with an unknown action ID, or with a target that does not match the receiving board is rejected without touching GPIO.

## Graph activation

The active graph is immutable. An update is written and validated in an inactive storage slot while the active graph continues operating.

Activation occurs at one main-loop boundary:

1. Stop accepting new evaluations from the old graph.
2. Preserve already transmitted actions; cancel unsent delayed invocations from the old generation.
3. Load the new local execution section.
4. Establish current input levels as the new baseline.
5. Reset graph-local event and timer state.
6. Resume evaluation under the new generation.

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