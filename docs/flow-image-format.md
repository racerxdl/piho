# Flow source and graph image format

This document defines the authoring contract and canonical binary representation for graph format 1. Runtime behavior is defined separately in [flow-executor-semantics.md](flow-executor-semantics.md).

All examples and fixtures are synthetic. Production exports, names, topics, credentials, mappings, and installation topology must remain outside this public repository.

## Authoring source

The compiler accepts strict UTF-8 JSON conforming to [`tools/piho-flow/schema/flow.schema.json`](../tools/piho-flow/schema/flow.schema.json). Duplicate JSON keys, unknown properties, implicit type coercion, invalid references, and unsupported primitives are errors.

The root fields are:

- `format`: graph source and image format; exactly `1`.
- `executorApi`: minimum firmware executor API; exactly `1` for this compiler.
- `generation`: nonzero unsigned 32-bit deployment generation.
- `devices`: symbolic board names mapped to physical ID and `input` or `output` role.
- `inputs`: symbolic inputs mapped to one input board, local pin, and debounce interval.
- `actions`: symbolic actions mapped to one output board, local pin, operation, and timing parameters.
- `flows`: one or more source triggers and one or more referenced actions.

Symbolic names are ASCII identifiers matching `^[A-Za-z][A-Za-z0-9_-]{0,63}$`. They exist only in source files and compiler diagnostics. They are not written to the runtime image.

Example:

```json
{
  "format": 1,
  "executorApi": 1,
  "generation": 7,
  "devices": {
    "input_a": { "id": 1, "role": "input" },
    "output_a": { "id": 7, "role": "output" }
  },
  "inputs": {
    "button_a": { "device": "input_a", "pin": 2, "debounceMs": 25 }
  },
  "actions": {
    "relay_a": { "device": "output_a", "pin": 3, "operation": "toggle" }
  },
  "flows": {
    "button_to_relay": {
      "when": [{ "input": "button_a", "edge": "rising" }],
      "actions": ["relay_a"]
    }
  }
}
```

A `set` action requires Boolean `value`. A `pulse` action requires `durationMs`. `copy_source` and `toggle` accept neither field. `delayMs` is optional and defaults to zero. A `copy_source` action may only be referenced by `changed` triggers.

Multiple `when` entries are independent fan-in routes. Multiple `actions` entries are fan-out. Executor API 1 has no flow-to-flow link, expression, or script field, so a cycle cannot be represented; attempts to add such a field fail as an unsupported property rather than being approximated.

## CLI

Run from `tools/piho-flow`:

```text
bun run flow validate ../../path/to/flow.json
bun run flow compile ../../path/to/flow.json --output /tmp/graph.phg
bun run flow inspect /tmp/graph.phg
```

`validate` and `compile` also accept `--executor-api N`. A source requiring an API newer than that target is rejected before image creation. Neither command connects to a device.

`inspect` fully validates the magic, versions, bounds, canonical offsets, reserved fields, references, ownership, operation parameters, per-device slices, and CRC before printing JSON. Its report includes the manifest, counts, section offsets, and section sizes.

## Canonical compilation

The compiler treats object declaration order, trigger order, and a flow's action-reference order as semantically irrelevant. It rejects duplicate entries, then applies these ordering rules:

1. Input IDs are assigned from `1` by symbolic input name in bytewise ASCII order.
2. Action IDs are assigned from `1` by symbolic action name in bytewise ASCII order.
3. Flow IDs are assigned from `1` by symbolic flow name in bytewise ASCII order.
4. Triggers in a flow are ordered by input name, then `rising`, `falling`, `changed`.
5. Action references in a route are ordered by action ID.
6. Device records are ordered by numeric device ID.
7. Inputs, routes, and actions are grouped by their owning device ID, then ordered by assigned ID.

A route ID identifies one compiled source node: one flow, input, and edge. Route IDs are assigned before device grouping, using the canonical flow and trigger order. These rules make semantically identical source documents produce byte-identical images and checksums.

## Integer and checksum rules

Every multi-byte integer is unsigned, little-endian. Every reserved byte is zero. Offsets and slice starts are zero-based indices unless explicitly identified as byte offsets.

The checksum is CRC-32/ISO-HDLC (polynomial `0x04C11DB7`, reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, reflected input/output, final XOR `0xFFFFFFFF`). It covers the complete image while bytes `52..55` of the checksum field are treated as zero.

## Header

The header is exactly 56 bytes.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | ASCII magic `PHGF` |
| 4 | 2 | Graph format version |
| 6 | 2 | Required executor API |
| 8 | 4 | Generation |
| 12 | 4 | Complete image size in bytes |
| 16 | 2 | Device record count |
| 18 | 2 | Input record count |
| 20 | 2 | Flow count |
| 22 | 2 | Route record count |
| 24 | 2 | Action-reference count |
| 26 | 2 | Action record count |
| 28 | 4 | Reserved, zero |
| 32 | 4 | Device directory byte offset |
| 36 | 4 | Input table byte offset |
| 40 | 4 | Route table byte offset |
| 44 | 4 | Action-reference table byte offset |
| 48 | 4 | Action table byte offset |
| 52 | 4 | CRC-32 |

Sections are contiguous in the order shown. Format 1 permits no padding or trailing bytes. Therefore all offsets and the image length are derivable from record counts and sizes; a decoder must reject noncanonical values.

## Device directory record

One 16-byte record exists for every declared board. The directory retains the global manifest and identifies each board's contiguous local execution sections.

| Record offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Device ID, `0..31` |
| 1 | 1 | Role: `1` input, `2` output |
| 2 | 2 | First input-table index |
| 4 | 2 | Input count |
| 6 | 2 | First route-table index |
| 8 | 2 | Route count |
| 10 | 2 | First action-table index |
| 12 | 2 | Action count |
| 14 | 2 | Reserved, zero |

A zero-count slice has start index zero. Input boards own inputs and routes. Output boards own actions. A decoder verifies that the slices canonically partition every record by owning device.

## Input record

Each input record is 8 bytes.

| Record offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | Stable input ID, nonzero |
| 2 | 1 | Owning input device ID |
| 3 | 1 | Local pin, `0..15` |
| 4 | 2 | Debounce interval in milliseconds |
| 6 | 2 | Reserved, zero |

## Route record

Each route record is 12 bytes.

| Record offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | Stable route ID, nonzero |
| 2 | 2 | Stable flow ID, nonzero |
| 4 | 2 | Source input ID |
| 6 | 2 | First action-reference table index |
| 8 | 1 | Owning source device ID |
| 9 | 1 | Edge: `1` rising, `2` falling, `3` changed |
| 10 | 1 | Action-reference count, `1..16` |
| 11 | 1 | Reserved, zero |

Action-reference slices follow route-table order with no gaps. Each action reference is one little-endian 16-bit action ID.

## Action record

Each action record is 16 bytes.

| Record offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | Stable action ID, nonzero |
| 2 | 1 | Target output device ID |
| 3 | 1 | Local pin, `0..15` |
| 4 | 1 | Operation: `1` set, `2` copy source, `3` toggle, `4` pulse |
| 5 | 1 | `set` value as `0` or `1`; zero for other operations |
| 6 | 2 | Reserved, zero |
| 8 | 4 | Dispatch delay in milliseconds |
| 12 | 4 | Pulse duration in milliseconds; zero for other operations |

## Capacity validation

The compiler enforces all static format-1 limits before returning an image: graph bytes, global table counts, local input/route/action counts, action references per route, pins, timers encoded in action parameters, role compatibility, references, and source ownership. Runtime concurrency limits for delayed or unacknowledged invocations are enforced by the executor because event arrival rate is not a static graph property; overflow remains observable and never changes the graph.
