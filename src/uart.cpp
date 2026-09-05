#include "uart.h"

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "global.h"
#include "io.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "piho.h"
#include "piho/addressing.h"
#include "piho/serial_framer.h"
#include "shift.pb.h"
#include "storage.h"

namespace {

piho::SerialFrameParser parser;

bool sendDeviceEvent(const DeviceEvent &event) {
    uint8_t payload[DeviceEvent_size]{};
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    if (!pb_encode(&stream, DeviceEvent_fields, &event)) {
        signalError();
        return false;
    }

    uint8_t encoded[piho::kSerialFrameCapacity]{};
    std::size_t encodedLength = 0;
    if (!piho::SerialFrameEncoder::encode(payload, static_cast<uint16_t>(stream.bytes_written), encoded,
                                          sizeof(encoded), encodedLength) ||
        Serial.write(encoded, encodedLength) != encodedLength) {
        signalError();
        return false;
    }
    return true;
}

ErrorEvent_Code errorCode(DeviceErrorCode code) {
    switch (code) {
        case DeviceErrorCode::InvalidFrame:
            return ErrorEvent_Code_INVALID_FRAME;
        case DeviceErrorCode::InvalidCommand:
            return ErrorEvent_Code_INVALID_COMMAND;
        case DeviceErrorCode::OutOfRange:
            return ErrorEvent_Code_OUT_OF_RANGE;
        case DeviceErrorCode::Transport:
            return ErrorEvent_Code_TRANSPORT;
        case DeviceErrorCode::Storage:
            return ErrorEvent_Code_STORAGE;
        case DeviceErrorCode::TriggerTableFull:
            return ErrorEvent_Code_TRIGGER_TABLE_FULL;
    }
    return ErrorEvent_Code_UNKNOWN;
}

AckEvent_Operation operationCode(DeviceOperation operation) {
    switch (operation) {
        case DeviceOperation::HealthCheck:
            return AckEvent_Operation_HEALTH_CHECK;
        case DeviceOperation::SetPin:
            return AckEvent_Operation_SET_PIN;
        case DeviceOperation::SetByte:
            return AckEvent_Operation_SET_BYTE;
        case DeviceOperation::Reset:
            return AckEvent_Operation_RESET;
        case DeviceOperation::UpsertTrigger:
            return AckEvent_Operation_UPSERT_TRIGGER;
        case DeviceOperation::RemoveTrigger:
            return AckEvent_Operation_REMOVE_TRIGGER;
        case DeviceOperation::ClearTriggers:
            return AckEvent_Operation_CLEAR_TRIGGERS;
    }
    return AckEvent_Operation_NONE;
}

bool validTriggerCommand(const TriggerCommand &command) {
    return command.input_device < piho::kDeviceCount && command.input_pin < piho::kPinsPerDevice &&
           command.output_device < piho::kDeviceCount && command.output_pin < piho::kPinsPerDevice;
}

void processCommand(PihoController &controller, const uint8_t *payload, uint16_t payloadLength) {
    HostCommand command = HostCommand_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payloadLength);
    if (!pb_decode(&stream, HostCommand_fields, &command)) {
        sendErrorEvent(DeviceErrorCode::InvalidCommand);
        return;
    }

    switch (command.which_command) {
        case HostCommand_health_check_tag:
            sendAckEvent(DeviceOperation::HealthCheck, controller.broadcastHealthCheck());
            return;
        case HostCommand_set_pin_tag: {
            const IndexedValue &request = command.command.set_pin;
            if (request.index >= piho::kGlobalPinCount || request.value > 1) {
                sendErrorEvent(DeviceErrorCode::OutOfRange);
                return;
            }
            const bool accepted = controller.setGlobalPin(static_cast<uint16_t>(request.index), request.value != 0);
            sendAckEvent(DeviceOperation::SetPin, accepted);
            return;
        }
        case HostCommand_set_byte_tag: {
            const IndexedValue &request = command.command.set_byte;
            if (request.index >= piho::kGlobalByteCount || request.value > UINT8_MAX) {
                sendErrorEvent(DeviceErrorCode::OutOfRange);
                return;
            }
            const bool accepted =
                controller.setGlobalByte(static_cast<uint16_t>(request.index), static_cast<uint8_t>(request.value));
            sendAckEvent(DeviceOperation::SetByte, accepted);
            return;
        }
        case HostCommand_reset_tag: {
            const ResetCommand &request = command.command.reset;
            if (!request.broadcast && request.device >= piho::kDeviceCount) {
                sendErrorEvent(DeviceErrorCode::OutOfRange);
                return;
            }
            const bool accepted = controller.requestReset(static_cast<uint8_t>(request.device), request.broadcast);
            sendAckEvent(DeviceOperation::Reset, accepted);
            return;
        }
        case HostCommand_status_tag:
            sendStatusEvent(controller);
            return;
        case HostCommand_upsert_trigger_tag:
        case HostCommand_remove_trigger_tag: {
            const bool upsert = command.which_command == HostCommand_upsert_trigger_tag;
            const TriggerCommand &request =
                upsert ? command.command.upsert_trigger : command.command.remove_trigger;
            if (!validTriggerCommand(request)) {
                sendErrorEvent(DeviceErrorCode::OutOfRange);
                return;
            }
            const piho::TriggerRule rule{static_cast<uint8_t>(request.input_device),
                                         static_cast<uint8_t>(request.input_pin),
                                         static_cast<uint8_t>(request.output_pin)};
            const bool accepted =
                upsert ? controller.upsertTrigger(static_cast<uint8_t>(request.output_device), rule)
                       : controller.removeTrigger(static_cast<uint8_t>(request.output_device), rule);
            sendAckEvent(upsert ? DeviceOperation::UpsertTrigger : DeviceOperation::RemoveTrigger, accepted);
            return;
        }
        case HostCommand_clear_triggers_tag: {
            const uint32_t device = command.command.clear_triggers.device;
            if (device >= piho::kDeviceCount) {
                sendErrorEvent(DeviceErrorCode::OutOfRange);
                return;
            }
            sendAckEvent(DeviceOperation::ClearTriggers, controller.clearTriggers(static_cast<uint8_t>(device)));
            return;
        }
        default:
            sendErrorEvent(DeviceErrorCode::InvalidCommand);
            return;
    }
}

}  // namespace

void handleUART(PihoController &controller) {
    for (std::size_t processed = 0; processed < MAX_UART_BYTES_PER_POLL && Serial.available() > 0; ++processed) {
        const int value = Serial.read();
        if (value < 0) {
            return;
        }

        piho::SerialFrameView frame{};
        const piho::FrameParseStatus status = parser.push(static_cast<uint8_t>(value), frame);
        if (status == piho::FrameParseStatus::Complete) {
            processCommand(controller, frame.data, frame.length);
        } else if (status != piho::FrameParseStatus::None) {
            sendErrorEvent(DeviceErrorCode::InvalidFrame);
        }
    }
}

void sendInputStateEvent(uint8_t device, uint16_t state) {
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_input_state_tag;
    event.event.input_state.device = device;
    event.event.input_state.state = state;
    sendDeviceEvent(event);
}

void sendStatusEvent(const PihoController &controller) {
    const CanTransportStats stats = controller.transportStats();
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_status_tag;
    event.event.status.device = controller.deviceId();
#ifdef IS_INPUT_DEVICE
    event.event.status.input_device = true;
    event.event.status.gpio_state = inputState();
    event.event.status.trigger_count = 0;
#else
    event.event.status.input_device = false;
    event.event.status.gpio_state = outputState();
    event.event.status.trigger_count = triggerRules.size();
#endif
    event.event.status.rx_dropped = stats.rxDropped;
    event.event.status.tx_dropped = stats.txDropped;
    event.event.status.bus_errors = stats.busErrors;
    sendDeviceEvent(event);
}

void sendErrorEvent(DeviceErrorCode code) {
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_error_tag;
    event.event.error.code = errorCode(code);
    sendDeviceEvent(event);
}

void sendAckEvent(DeviceOperation operation, bool accepted) {
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_ack_tag;
    event.event.ack.operation = operationCode(operation);
    event.event.ack.accepted = accepted;
    sendDeviceEvent(event);
}
