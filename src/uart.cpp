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
        case DeviceErrorCode::GraphUpdate:
            return ErrorEvent_Code_GRAPH_UPDATE;
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
        case DeviceOperation::GraphBegin:
            return AckEvent_Operation_GRAPH_BEGIN;
        case DeviceOperation::GraphChunk:
            return AckEvent_Operation_GRAPH_CHUNK;
        case DeviceOperation::GraphFinish:
            return AckEvent_Operation_GRAPH_FINISH;
        case DeviceOperation::GraphAbort:
            return AckEvent_Operation_GRAPH_ABORT;
        case DeviceOperation::GraphActivate:
            return AckEvent_Operation_GRAPH_ACTIVATE;
        case DeviceOperation::GraphRollback:
            return AckEvent_Operation_GRAPH_ROLLBACK;
        case DeviceOperation::GraphStatus:
            return AckEvent_Operation_GRAPH_STATUS;
    }
    return AckEvent_Operation_NONE;
}

void reportGraphResult(DeviceOperation operation, piho::GraphUpdateError result,
                       const piho::GraphUpdateCoordinator &graphUpdate) {
    const bool accepted = result == piho::GraphUpdateError::None;
    sendAckEvent(operation, accepted);
    piho::GraphGatewayStatus status = graphUpdate.status();
    if (!accepted) {
        status.lastError = result;
    }
    sendGraphUpdateEvent(status);
    if (!accepted) {
        sendErrorEvent(DeviceErrorCode::GraphUpdate);
    }
}

void processCommand(PihoController &controller,
                    piho::GraphUpdateCoordinator &graphUpdate,
                    const piho::GraphNodeUpdateStatus &nodeUpdate,
                    const piho::GraphRuntimeStatus &runtime,
                    const uint8_t *payload, uint16_t payloadLength) {
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
            sendStatusEvent(controller, nodeUpdate, runtime);
            return;
        case HostCommand_graph_begin_tag: {
            const GraphBeginCommand &request = command.command.graph_begin;
            piho::GraphUpdateError result = piho::GraphUpdateError::InvalidDescriptor;
            if (request.transfer_id <= UINT16_MAX && request.format <= UINT16_MAX &&
                request.executor_api <= UINT16_MAX) {
                piho::GraphTransferDescriptor descriptor{};
                descriptor.transferId = static_cast<uint16_t>(request.transfer_id);
                descriptor.format = static_cast<uint16_t>(request.format);
                descriptor.executorApi = static_cast<uint16_t>(request.executor_api);
                descriptor.generation = request.generation;
                descriptor.imageSize = request.image_size;
                descriptor.checksum = request.checksum;
                descriptor.expectedDevices = request.expected_devices;
                result = graphUpdate.beginUpdate(descriptor, millis());
            }
            reportGraphResult(DeviceOperation::GraphBegin, result, graphUpdate);
            return;
        }
        case HostCommand_graph_chunk_tag: {
            const GraphChunkCommand &request = command.command.graph_chunk;
            piho::GraphUpdateError result = piho::GraphUpdateError::InvalidDescriptor;
            if (request.transfer_id <= UINT16_MAX && request.sequence <= UINT16_MAX) {
                result = graphUpdate.queueChunk(
                    static_cast<uint16_t>(request.transfer_id),
                    static_cast<uint16_t>(request.sequence), request.data.bytes,
                    static_cast<uint8_t>(request.data.size), millis());
            }
            reportGraphResult(DeviceOperation::GraphChunk, result, graphUpdate);
            return;
        }
        case HostCommand_graph_finish_tag: {
            const GraphFinishCommand &request = command.command.graph_finish;
            piho::GraphUpdateError result = piho::GraphUpdateError::InvalidDescriptor;
            if (request.transfer_id <= UINT16_MAX &&
                request.sequence_count <= UINT16_MAX) {
                result = graphUpdate.finishUpdate(
                    static_cast<uint16_t>(request.transfer_id),
                    static_cast<uint16_t>(request.sequence_count), millis());
            }
            reportGraphResult(DeviceOperation::GraphFinish, result, graphUpdate);
            return;
        }
        case HostCommand_graph_abort_tag: {
            const uint32_t transferId = command.command.graph_abort.transfer_id;
            const piho::GraphUpdateError result =
                transferId <= UINT16_MAX
                    ? graphUpdate.abortUpdate(static_cast<uint16_t>(transferId), millis())
                    : piho::GraphUpdateError::InvalidDescriptor;
            reportGraphResult(DeviceOperation::GraphAbort, result, graphUpdate);
            return;
        }
        case HostCommand_graph_activate_tag:
            reportGraphResult(DeviceOperation::GraphActivate,
                              graphUpdate.activateUpdate(millis()), graphUpdate);
            return;
        case HostCommand_graph_rollback_tag: {
            const GraphIdentityCommand &request = command.command.graph_rollback;
            const piho::GraphIdentity target{0, 0, request.generation, request.checksum};
            reportGraphResult(
                DeviceOperation::GraphRollback,
                graphUpdate.rollbackUpdate(target, request.expected_devices, millis()),
                graphUpdate);
            return;
        }
        case HostCommand_graph_status_tag: {
            const uint32_t transferId = command.command.graph_status.transfer_id;
            const piho::GraphUpdateError result =
                transferId <= UINT16_MAX
                    ? graphUpdate.requestStatus(static_cast<uint16_t>(transferId), millis())
                    : piho::GraphUpdateError::InvalidDescriptor;
            reportGraphResult(DeviceOperation::GraphStatus, result, graphUpdate);
            return;
        }
        default:
            sendErrorEvent(DeviceErrorCode::InvalidCommand);
            return;
    }
}

}  // namespace

void handleUART(PihoController &controller,
                piho::GraphUpdateCoordinator &graphUpdate,
                const piho::GraphNodeUpdateStatus &nodeUpdate,
                const piho::GraphRuntimeStatus &runtime) {
    for (std::size_t processed = 0; processed < MAX_UART_BYTES_PER_POLL && Serial.available() > 0; ++processed) {
        const int value = Serial.read();
        if (value < 0) {
            return;
        }

        piho::SerialFrameView frame{};
        const piho::FrameParseStatus status = parser.push(static_cast<uint8_t>(value), frame);
        if (status == piho::FrameParseStatus::Complete) {
            processCommand(controller, graphUpdate, nodeUpdate, runtime,
                           frame.data, frame.length);
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

void sendStatusEvent(const PihoController &controller,
                     const piho::GraphNodeUpdateStatus &nodeUpdate,
                     const piho::GraphRuntimeStatus &runtime) {
    const CanTransportStats stats = controller.transportStats();
    const piho::GraphStoreStatus &stored = graphStore.status();
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_status_tag;
    StatusEvent &status = event.event.status;
    status.device = controller.deviceId();
#ifdef IS_INPUT_DEVICE
    status.input_device = true;
    status.gpio_state = inputState();
#else
    status.input_device = false;
    status.gpio_state = outputState();
#endif
    status.rx_dropped = stats.rxDropped;
    status.tx_dropped = stats.txDropped;
    status.bus_errors = stats.busErrors;
    status.graph_generation = runtime.identity.generation;
    status.graph_checksum = runtime.identity.checksum;
    status.update_state =
        static_cast<::GraphUpdateState>(nodeUpdate.state);
    status.store_state = static_cast<::GraphStoreState>(stored.state);
    status.store_error = static_cast<::GraphStoreError>(stored.lastError);
    status.flow_accepted_events = runtime.flowAcceptedEvents;
    status.flow_evaluated_actions = runtime.flowEvaluatedActions;
    status.action_retries = runtime.actionRetries;
    status.action_rejections = runtime.actionRejections;
    status.executor_executed_actions = runtime.executorExecutedActions;
    status.executor_rejected_actions = runtime.executorRejectedActions;
    sendDeviceEvent(event);
}

void sendGraphUpdateEvent(const piho::GraphGatewayStatus &status) {
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_graph_update_tag;
    GraphUpdateEvent &update = event.event.graph_update;
    update.transfer_id = status.descriptor.transferId;
    update.generation = status.descriptor.generation;
    update.checksum = status.descriptor.checksum;
    update.expected_devices = status.descriptor.expectedDevices;
    update.ready_devices = status.readyDevices;
    update.progressed_devices = status.progressedDevices;
    update.staged_devices = status.stagedDevices;
    update.rejected_devices = status.rejectedDevices;
    update.active_devices = status.activeDevices;
    update.rollback_devices = status.rollbackDevices;
    update.missing_devices = status.missingDevices;
    update.next_sequence = status.nextSequence;
    update.sequence_count = status.sequenceCount;
    update.state = static_cast<::GraphUpdateState>(status.state);
    update.error = static_cast<::GraphUpdateError>(status.lastError);
    update.chunk_pending = status.chunkPending;
    sendDeviceEvent(event);
}

void sendGraphNodeStatusEvent(const piho::ProtocolMessage &message) {
    DeviceEvent event = DeviceEvent_init_zero;
    event.which_event = DeviceEvent_graph_node_status_tag;
    GraphNodeStatusEvent &status = event.event.graph_node_status;
    status.device = message.device;
    switch (message.type) {
        case piho::MessageType::GraphNodeCapabilities:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_CAPABILITIES;
            status.format = message.graphNode.format;
            status.executor_api = message.graphNode.executorApi;
            status.role = static_cast<::DeviceRole>(message.graphNode.role);
            break;
        case piho::MessageType::GraphNodeState:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_STATE;
            status.transfer_id = message.graphNode.transferId;
            status.update_state =
                static_cast<::GraphUpdateState>(message.graphNode.updateState);
            status.update_error =
                static_cast<::GraphUpdateError>(message.graphNode.updateError);
            status.store_state =
                static_cast<::GraphStoreState>(message.graphNode.storeState);
            status.store_error =
                static_cast<::GraphStoreError>(message.graphNode.storeError);
            break;
        case piho::MessageType::GraphActiveIdentity:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_ACTIVE;
            status.generation = message.graphNode.identity.generation;
            status.checksum = message.graphNode.identity.checksum;
            break;
        case piho::MessageType::GraphStagedIdentity:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_STAGED;
            status.generation = message.graphNode.identity.generation;
            status.checksum = message.graphNode.identity.checksum;
            break;
        case piho::MessageType::GraphRollbackIdentity:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_ROLLBACK;
            status.generation = message.graphNode.identity.generation;
            status.checksum = message.graphNode.identity.checksum;
            break;
        case piho::MessageType::GraphManifestStatus:
            status.part = GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_MANIFESTS;
            status.active_devices = message.graphNode.activeDevices;
            status.staged_devices = message.graphNode.stagedDevices;
            break;
        case piho::MessageType::GraphRollbackManifest:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_ROLLBACK_MANIFEST;
            status.rollback_devices = message.graphNode.rollbackDevices;
            break;
        case piho::MessageType::GraphTransportDrops:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_TRANSPORT_DROPS;
            status.rx_dropped = message.graphNode.rxDropped;
            status.tx_dropped = message.graphNode.txDropped;
            break;
        case piho::MessageType::GraphTransportErrors:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_TRANSPORT_ERRORS;
            status.bus_errors = message.graphNode.busErrors;
            break;
        case piho::MessageType::GraphRuntimeIdentity:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_RUNTIME_IDENTITY;
            status.runtime_generation =
                message.graphNode.identity.generation;
            status.runtime_checksum = message.graphNode.identity.checksum;
            break;
        case piho::MessageType::GraphFlowCounters:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_FLOW_COUNTERS;
            status.flow_accepted_events =
                message.graphNode.flowAcceptedEvents;
            status.flow_evaluated_actions =
                message.graphNode.flowEvaluatedActions;
            break;
        case piho::MessageType::GraphActionCounters:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_ACTION_COUNTERS;
            status.action_retries = message.graphNode.actionRetries;
            status.action_rejections = message.graphNode.actionRejections;
            break;
        case piho::MessageType::GraphExecutorCounters:
            status.part =
                GraphNodeStatusPart_GRAPH_NODE_STATUS_PART_EXECUTOR_COUNTERS;
            status.executor_executed_actions =
                message.graphNode.executorExecutedActions;
            status.executor_rejected_actions =
                message.graphNode.executorRejectedActions;
            break;
        default:
            return;
    }
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
