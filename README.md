# Piho

RP2040 home-automation input and output nodes connected by CAN bus.

Piho builds two Raspberry Pi Pico firmware variants:

- `input_device`: debounces 16 active-low GPIO inputs and publishes state changes.
- `output_device`: controls 16 active-low GPIO outputs and applies persistent rising-edge trigger rules.

## Compatibility

The current firmware uses protocol version 1. It is a clean wire-protocol cutover and is not compatible with earlier Piho firmware or the old unframed USB commands. Reflash every node on a CAN network together and update the host from [`protocol/shift.proto`](protocol/shift.proto).

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

The five pulled-up address pins form a device ID from 0 through 31. Input and output polarity, CAN bitrate, debounce time, and pin assignments are defined in [`include/config.h`](include/config.h).

## Build and test

Requirements: Git with submodule support and PlatformIO Core.

```sh
git submodule update --init --recursive
pio run -e input_device
pio run -e output_device
pio test -e native
```

The PlatformIO platform, Nanopb, native test platform, and `can2040` submodule are pinned. Nanopb generates `shift.pb.c` and `shift.pb.h` into `.pio/build`; generated files are not source-controlled.

Firmware images are written to:

- `.pio/build/input_device/firmware.uf2`
- `.pio/build/output_device/firmware.uf2`

## CAN protocol

Piho accepts only 29-bit extended data frames in its namespace:

```text
(0x150 << 20) | (version << 16) | (message_type << 8) | device
```

Version is `1`. Physical device IDs are `0..31`; `0xFF` is accepted only for health-check and reset broadcasts. Every message has an exact payload length:

| Type | Payload |
| --- | --- |
| Health check | empty |
| Reset | empty |
| Input state | `uint16` state, little-endian |
| Output state | `uint16` state, little-endian |
| Set pin | local pin, boolean value |
| Set byte | local byte, value |
| Upsert/remove trigger | input device, input pin, local output pin |
| Clear triggers | empty |

Frames with another namespace, remote-frame flag, unsupported type, invalid device, incorrect length, or invalid payload are rejected before dispatch.

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

The version is `1`, the payload limit is 128 bytes, and CRC-16/CCITT uses polynomial `0x1021` with initial value `0xFFFF`. The checksum covers version, encoded length, and payload. Host-to-device payloads are `HostCommand`; responses and asynchronous updates are `DeviceEvent`. See [`protocol/shift.proto`](protocol/shift.proto) for the command and event schema.

The parser consumes a bounded number of bytes per firmware loop, validates framing before protobuf decoding, and resynchronizes after malformed input.

## Triggers and storage

An output node stores up to 128 trigger rules. A rule maps one input device and pin to one local output pin. The output toggles only on a debounced rising edge; the first state observed from each input device establishes a baseline and does not toggle outputs.

Rules are encoded explicitly with a version and CRC32, then saved to alternating LittleFS slots. A write is read back and verified before it becomes active. Invalid or interrupted writes leave the previous valid generation available.

The legacy `/triggers.bin` layout is not migrated. The first version-1 boot creates the new redundant store and removes that file, so existing trigger rules must be configured again.

## License

Project-authored source is licensed under Apache-2.0; see [`LICENSE`](LICENSE).

The current firmware also links the `lib/can2040` submodule, which is licensed under GPLv3. Distributed firmware binaries therefore require compliance with the GPLv3 terms applicable to that linked work. The transport is isolated behind `CanTransport` to permit a future replacement, but `can2040` remains in use for hardware compatibility. See [`lib/can2040/COPYING`](lib/can2040/COPYING) and [can2040 issue #5](https://github.com/KevinOConnor/can2040/issues/5).
