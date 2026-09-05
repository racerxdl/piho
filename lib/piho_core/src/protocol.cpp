#include "piho/protocol.h"

#include <cstring>

namespace piho {
namespace {

constexpr uint8_t messageType(uint32_t identifier) {
    return static_cast<uint8_t>((identifier >> 8) & 0xFFu);
}

constexpr uint8_t messageDevice(uint32_t identifier) {
    return static_cast<uint8_t>(identifier & 0xFFu);
}

void encodeUint16(uint16_t value, uint8_t *output) {
    output[0] = static_cast<uint8_t>(value & 0xFFu);
    output[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void encodeUint32(uint32_t value, uint8_t *output) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        output[byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

uint16_t decodeUint16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

uint32_t decodeUint32(const uint8_t *input) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(input[byte]) << (byte * 8);
    }
    return value;
}

bool validAckStatus(ActionAckStatus status) {
    return status == ActionAckStatus::Executed || status == ActionAckStatus::AlreadyExecuted ||
           status == ActionAckStatus::WrongGeneration ||
           status == ActionAckStatus::UnknownAction || status == ActionAckStatus::WrongTarget ||
           status == ActionAckStatus::InvalidAction ||
           status == ActionAckStatus::UnavailableOutput;
}

bool validGraphState(GraphUpdateState state) {
    return state == GraphUpdateState::Idle || state == GraphUpdateState::Receiving ||
           state == GraphUpdateState::Validating || state == GraphUpdateState::Staged ||
           state == GraphUpdateState::Active || state == GraphUpdateState::Rollback ||
           state == GraphUpdateState::Rejected;
}

bool validGraphError(GraphUpdateError error) {
    return error >= GraphUpdateError::None && error <= GraphUpdateError::Aborted;
}

bool validTransferDescriptor(const GraphTransferDescriptor &descriptor) {
    return descriptor.transferId != 0 && descriptor.format != 0 &&
           descriptor.executorApi != 0 && descriptor.generation != 0 &&
           descriptor.imageSize >= kGraphHeaderSize &&
           descriptor.imageSize <= kGraphImageCapacity && descriptor.expectedDevices != 0;
}

bool acceptsBroadcast(MessageType type) {
    return type == MessageType::HealthCheck || type == MessageType::Reset;
}

bool validDevice(MessageType type, uint8_t encodedDevice) {
    if (type == MessageType::ActionAck) {
        return isPhysicalDevice(static_cast<uint8_t>(encodedDevice & 0x1Fu));
    }
    if (type == MessageType::GraphStatusIdentity ||
        type == MessageType::GraphStatusProgress) {
        return isPhysicalDevice(encodedDevice);
    }
    if (static_cast<uint8_t>(type) >= kGraphTransferMessageTypeMinimum) {
        return encodedDevice == kBroadcastDevice;
    }
    return isPhysicalDevice(encodedDevice) ||
           (encodedDevice == kBroadcastDevice && acceptsBroadcast(type));
}

uint8_t expectedLength(MessageType type) {
    switch (type) {
        case MessageType::HealthCheck:
        case MessageType::Reset:
        case MessageType::ClearTriggers:
            return 0;
        case MessageType::InputState:
        case MessageType::OutputState:
        case MessageType::SetPin:
        case MessageType::SetByte:
        case MessageType::GraphAbort:
        case MessageType::GraphStatusRequest:
            return 2;
        case MessageType::UpsertTrigger:
        case MessageType::RemoveTrigger:
            return 3;
        case MessageType::GraphFinish:
            return 4;
        case MessageType::ExecuteAction:
        case MessageType::ActionAck:
        case MessageType::GraphBegin:
        case MessageType::GraphCompatibility:
        case MessageType::GraphDevices:
        case MessageType::GraphChecksum:
        case MessageType::GraphActivate:
        case MessageType::GraphRollback:
        case MessageType::GraphStatusIdentity:
        case MessageType::GraphStatusProgress:
            return 8;
        case MessageType::GraphChunk:
            return 0xFE;
    }
    return 0xFF;
}

bool knownType(uint8_t rawType, MessageType &type) {
    switch (static_cast<MessageType>(rawType)) {
        case MessageType::HealthCheck:
        case MessageType::Reset:
        case MessageType::InputState:
        case MessageType::OutputState:
        case MessageType::SetPin:
        case MessageType::SetByte:
        case MessageType::UpsertTrigger:
        case MessageType::RemoveTrigger:
        case MessageType::ClearTriggers:
        case MessageType::ExecuteAction:
        case MessageType::ActionAck:
        case MessageType::GraphBegin:
        case MessageType::GraphCompatibility:
        case MessageType::GraphDevices:
        case MessageType::GraphChecksum:
        case MessageType::GraphChunk:
        case MessageType::GraphFinish:
        case MessageType::GraphAbort:
        case MessageType::GraphActivate:
        case MessageType::GraphRollback:
        case MessageType::GraphStatusRequest:
        case MessageType::GraphStatusIdentity:
        case MessageType::GraphStatusProgress:
            type = static_cast<MessageType>(rawType);
            return true;
    }
    return false;
}

}  // namespace

CanFrame ProtocolCodec::makeFrame(MessageType type, uint8_t device) {
    CanFrame frame{};
    frame.identifier = kProtocolHeader | (static_cast<uint32_t>(type) << 8) | device;
    frame.extended = true;
    return frame;
}

CanFrame ProtocolCodec::healthCheck(uint8_t device) {
    return makeFrame(MessageType::HealthCheck, device);
}

CanFrame ProtocolCodec::reset(uint8_t device) {
    return makeFrame(MessageType::Reset, device);
}

bool ProtocolCodec::inputState(uint8_t sourceDevice, uint16_t state, CanFrame &frame) {
    if (!isPhysicalDevice(sourceDevice)) {
        return false;
    }
    frame = makeFrame(MessageType::InputState, sourceDevice);
    frame.length = 2;
    encodeUint16(state, frame.data);
    return true;
}

bool ProtocolCodec::outputState(uint8_t targetDevice, uint16_t state, CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice)) {
        return false;
    }
    frame = makeFrame(MessageType::OutputState, targetDevice);
    frame.length = 2;
    encodeUint16(state, frame.data);
    return true;
}

bool ProtocolCodec::setPin(uint8_t targetDevice, uint8_t localPin, bool value, CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice) || localPin >= kPinsPerDevice) {
        return false;
    }
    frame = makeFrame(MessageType::SetPin, targetDevice);
    frame.length = 2;
    frame.data[0] = localPin;
    frame.data[1] = value ? 1 : 0;
    return true;
}

bool ProtocolCodec::setByte(uint8_t targetDevice, uint8_t localByte, uint8_t value,
                            CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice) || localByte >= kBytesPerDevice) {
        return false;
    }
    frame = makeFrame(MessageType::SetByte, targetDevice);
    frame.length = 2;
    frame.data[0] = localByte;
    frame.data[1] = value;
    return true;
}

bool ProtocolCodec::upsertTrigger(uint8_t targetDevice, const TriggerRule &rule,
                                  CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice) || !isValid(rule)) {
        return false;
    }
    frame = makeFrame(MessageType::UpsertTrigger, targetDevice);
    frame.length = 3;
    frame.data[0] = rule.inputDevice;
    frame.data[1] = rule.inputPin;
    frame.data[2] = rule.outputPin;
    return true;
}

bool ProtocolCodec::removeTrigger(uint8_t targetDevice, const TriggerRule &rule,
                                  CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice) || !isValid(rule)) {
        return false;
    }
    frame = makeFrame(MessageType::RemoveTrigger, targetDevice);
    frame.length = 3;
    frame.data[0] = rule.inputDevice;
    frame.data[1] = rule.inputPin;
    frame.data[2] = rule.outputPin;
    return true;
}

bool ProtocolCodec::clearTriggers(uint8_t targetDevice, CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice)) {
        return false;
    }
    frame = makeFrame(MessageType::ClearTriggers, targetDevice);
    return true;
}

bool ProtocolCodec::executeAction(const ActionRequest &request, CanFrame &frame) {
    if (request.generation == 0 || request.eventToken == 0 ||
        request.eventToken > kActionEventTokenMaximum || request.actionId == 0 ||
        request.actionId > kGraphActionCapacity || !isPhysicalDevice(request.sourceDevice) ||
        !isPhysicalDevice(request.targetDevice)) {
        return false;
    }
    frame = makeFrame(MessageType::ExecuteAction, request.targetDevice);
    frame.length = 8;
    encodeUint32(request.generation, frame.data);
    const uint32_t packed =
        static_cast<uint32_t>(request.actionId - 1) | (request.eventToken << 9) |
        (static_cast<uint32_t>(request.sourceDevice) << 26) |
        (request.sourceValue ? 0x80000000u : 0u);
    encodeUint32(packed, &frame.data[4]);
    return true;
}

bool ProtocolCodec::actionAcknowledgement(const ActionAcknowledgement &acknowledgement,
                                          CanFrame &frame) {
    if (acknowledgement.generation == 0 || acknowledgement.eventToken == 0 ||
        acknowledgement.eventToken > kActionEventTokenMaximum ||
        acknowledgement.actionId == 0 || acknowledgement.actionId > kGraphActionCapacity ||
        !isPhysicalDevice(acknowledgement.sourceDevice) ||
        !isPhysicalDevice(acknowledgement.outputDevice) ||
        !validAckStatus(acknowledgement.status)) {
        return false;
    }
    const uint8_t encodedDevice =
        static_cast<uint8_t>(acknowledgement.sourceDevice |
                             (static_cast<uint8_t>(acknowledgement.status) << 5));
    frame = makeFrame(MessageType::ActionAck, encodedDevice);
    frame.length = 8;
    encodeUint32(acknowledgement.generation, frame.data);
    const uint32_t packed =
        static_cast<uint32_t>(acknowledgement.actionId - 1) |
        (acknowledgement.eventToken << 9) |
        (static_cast<uint32_t>(acknowledgement.outputDevice) << 26);
    encodeUint32(packed, &frame.data[4]);
    return true;
}

bool ProtocolCodec::graphBegin(const GraphTransferDescriptor &descriptor, CanFrame &frame) {
    if (!validTransferDescriptor(descriptor)) {
        return false;
    }
    frame = makeFrame(MessageType::GraphBegin, kBroadcastDevice);
    frame.length = 8;
    encodeUint16(descriptor.transferId, frame.data);
    encodeUint32(descriptor.generation, &frame.data[2]);
    encodeUint16(static_cast<uint16_t>(descriptor.imageSize), &frame.data[6]);
    return true;
}

bool ProtocolCodec::graphCompatibility(const GraphTransferDescriptor &descriptor,
                                       CanFrame &frame) {
    if (!validTransferDescriptor(descriptor)) {
        return false;
    }
    frame = makeFrame(MessageType::GraphCompatibility, kBroadcastDevice);
    frame.length = 8;
    encodeUint16(descriptor.transferId, frame.data);
    encodeUint16(descriptor.format, &frame.data[2]);
    encodeUint16(descriptor.executorApi, &frame.data[4]);
    return true;
}

bool ProtocolCodec::graphDevices(const GraphTransferDescriptor &descriptor, CanFrame &frame) {
    if (!validTransferDescriptor(descriptor)) {
        return false;
    }
    frame = makeFrame(MessageType::GraphDevices, kBroadcastDevice);
    frame.length = 8;
    encodeUint16(descriptor.transferId, frame.data);
    encodeUint32(descriptor.expectedDevices, &frame.data[2]);
    return true;
}

bool ProtocolCodec::graphChecksum(const GraphTransferDescriptor &descriptor, CanFrame &frame) {
    if (!validTransferDescriptor(descriptor)) {
        return false;
    }
    frame = makeFrame(MessageType::GraphChecksum, kBroadcastDevice);
    frame.length = 8;
    encodeUint16(descriptor.transferId, frame.data);
    encodeUint32(descriptor.checksum, &frame.data[2]);
    return true;
}

bool ProtocolCodec::graphChunk(uint16_t transferId, uint16_t sequence, const uint8_t *data,
                               uint8_t size, CanFrame &frame) {
    if (transferId == 0 || sequence >= kGraphChunkSequenceCapacity || data == nullptr ||
        size == 0 || size > kGraphChunkDataCapacity) {
        return false;
    }
    frame = makeFrame(MessageType::GraphChunk, kBroadcastDevice);
    frame.length = static_cast<uint8_t>(4 + size);
    encodeUint16(transferId, frame.data);
    encodeUint16(sequence, &frame.data[2]);
    std::memcpy(&frame.data[4], data, size);
    return true;
}

bool ProtocolCodec::graphFinish(uint16_t transferId, uint16_t sequenceCount, CanFrame &frame) {
    if (transferId == 0 || sequenceCount == 0 ||
        sequenceCount > kGraphChunkSequenceCapacity) {
        return false;
    }
    frame = makeFrame(MessageType::GraphFinish, kBroadcastDevice);
    frame.length = 4;
    encodeUint16(transferId, frame.data);
    encodeUint16(sequenceCount, &frame.data[2]);
    return true;
}

bool ProtocolCodec::graphAbort(uint16_t transferId, CanFrame &frame) {
    if (transferId == 0) {
        return false;
    }
    frame = makeFrame(MessageType::GraphAbort, kBroadcastDevice);
    frame.length = 2;
    encodeUint16(transferId, frame.data);
    return true;
}

bool ProtocolCodec::graphActivate(const GraphIdentity &identity, CanFrame &frame) {
    if (identity.generation == 0) {
        return false;
    }
    frame = makeFrame(MessageType::GraphActivate, kBroadcastDevice);
    frame.length = 8;
    encodeUint32(identity.generation, frame.data);
    encodeUint32(identity.checksum, &frame.data[4]);
    return true;
}

bool ProtocolCodec::graphRollback(const GraphIdentity &identity, CanFrame &frame) {
    if (identity.generation == 0) {
        return false;
    }
    frame = makeFrame(MessageType::GraphRollback, kBroadcastDevice);
    frame.length = 8;
    encodeUint32(identity.generation, frame.data);
    encodeUint32(identity.checksum, &frame.data[4]);
    return true;
}

bool ProtocolCodec::graphStatusRequest(uint16_t transferId, CanFrame &frame) {
    frame = makeFrame(MessageType::GraphStatusRequest, kBroadcastDevice);
    frame.length = 2;
    encodeUint16(transferId, frame.data);
    return true;
}

bool ProtocolCodec::graphStatusIdentity(uint8_t sourceDevice, uint16_t transferId,
                                        uint32_t generation, GraphUpdateState state,
                                        GraphUpdateError error, CanFrame &frame) {
    if (!isPhysicalDevice(sourceDevice) || !validGraphState(state) ||
        !validGraphError(error)) {
        return false;
    }
    frame = makeFrame(MessageType::GraphStatusIdentity, sourceDevice);
    frame.length = 8;
    encodeUint16(transferId, frame.data);
    encodeUint32(generation, &frame.data[2]);
    frame.data[6] = static_cast<uint8_t>(state);
    frame.data[7] = static_cast<uint8_t>(error);
    return true;
}

bool ProtocolCodec::graphStatusProgress(uint8_t sourceDevice, uint16_t transferId,
                                        uint32_t checksum, uint16_t nextSequence,
                                        CanFrame &frame) {
    if (!isPhysicalDevice(sourceDevice) || nextSequence > kGraphChunkSequenceCapacity) {
        return false;
    }
    frame = makeFrame(MessageType::GraphStatusProgress, sourceDevice);
    frame.length = 8;
    encodeUint16(transferId, frame.data);
    encodeUint32(checksum, &frame.data[2]);
    encodeUint16(nextSequence, &frame.data[6]);
    return true;
}

DecodeResult ProtocolCodec::decode(const CanFrame &frame) {
    DecodeResult result{};
    if (!frame.extended) {
        result.error = ProtocolError::NotExtended;
        return result;
    }
    if ((frame.identifier & ~kCanIdentifierMask) != 0 ||
        (frame.identifier & kProtocolHeaderMask) != kProtocolHeader) {
        result.error = ProtocolError::InvalidIdentifier;
        return result;
    }
    if (frame.remote) {
        result.error = ProtocolError::RemoteFrame;
        return result;
    }

    MessageType type;
    if (!knownType(messageType(frame.identifier), type)) {
        result.error = ProtocolError::UnsupportedType;
        return result;
    }

    const uint8_t device = messageDevice(frame.identifier);
    if (!validDevice(type, device)) {
        result.error = ProtocolError::InvalidDevice;
        return result;
    }
    const uint8_t length = expectedLength(type);
    if ((type == MessageType::GraphChunk &&
         (frame.length < 5 || frame.length > kCanPayloadCapacity)) ||
        (type != MessageType::GraphChunk && frame.length != length)) {
        result.error = ProtocolError::InvalidLength;
        return result;
    }

    result.message.type = type;
    result.message.device =
        type == MessageType::ActionAck ? static_cast<uint8_t>(device & 0x1Fu) : device;
    GraphTransferMessage &graph = result.message.graph;
    switch (type) {
        case MessageType::InputState:
        case MessageType::OutputState:
            result.message.state = decodeUint16(frame.data);
            break;
        case MessageType::SetPin:
            if (frame.data[0] >= kPinsPerDevice || frame.data[1] > 1) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            result.message.index = frame.data[0];
            result.message.value = frame.data[1];
            break;
        case MessageType::SetByte:
            if (frame.data[0] >= kBytesPerDevice) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            result.message.index = frame.data[0];
            result.message.value = frame.data[1];
            break;
        case MessageType::UpsertTrigger:
        case MessageType::RemoveTrigger:
            result.message.trigger = TriggerRule{frame.data[0], frame.data[1], frame.data[2]};
            if (!isValid(result.message.trigger)) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::ExecuteAction: {
            const uint32_t packed = decodeUint32(&frame.data[4]);
            ActionRequest &request = result.message.actionRequest;
            request.generation = decodeUint32(frame.data);
            request.actionId = static_cast<uint16_t>((packed & 0x1FFu) + 1);
            request.eventToken = (packed >> 9) & kActionEventTokenMaximum;
            request.sourceDevice = static_cast<uint8_t>((packed >> 26) & 0x1Fu);
            request.targetDevice = result.message.device;
            request.sourceValue = (packed & 0x80000000u) != 0;
            if (request.generation == 0 || request.eventToken == 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        }
        case MessageType::ActionAck: {
            const uint32_t packed = decodeUint32(&frame.data[4]);
            ActionAcknowledgement &acknowledgement = result.message.actionAcknowledgement;
            acknowledgement.generation = decodeUint32(frame.data);
            acknowledgement.actionId = static_cast<uint16_t>((packed & 0x1FFu) + 1);
            acknowledgement.eventToken = (packed >> 9) & kActionEventTokenMaximum;
            acknowledgement.sourceDevice = result.message.device;
            acknowledgement.outputDevice = static_cast<uint8_t>((packed >> 26) & 0x1Fu);
            acknowledgement.status = static_cast<ActionAckStatus>(device >> 5);
            if (acknowledgement.generation == 0 || acknowledgement.eventToken == 0 ||
                (packed & 0x80000000u) != 0 ||
                !validAckStatus(acknowledgement.status)) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        }
        case MessageType::GraphBegin:
            graph.descriptor.transferId = decodeUint16(frame.data);
            graph.descriptor.generation = decodeUint32(&frame.data[2]);
            graph.descriptor.imageSize = decodeUint16(&frame.data[6]);
            if (graph.descriptor.transferId == 0 || graph.descriptor.generation == 0 ||
                graph.descriptor.imageSize < kGraphHeaderSize ||
                graph.descriptor.imageSize > kGraphImageCapacity) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphCompatibility:
            graph.descriptor.transferId = decodeUint16(frame.data);
            graph.descriptor.format = decodeUint16(&frame.data[2]);
            graph.descriptor.executorApi = decodeUint16(&frame.data[4]);
            if (graph.descriptor.transferId == 0 || graph.descriptor.format == 0 ||
                graph.descriptor.executorApi == 0 || frame.data[6] != 0 ||
                frame.data[7] != 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphDevices:
            graph.descriptor.transferId = decodeUint16(frame.data);
            graph.descriptor.expectedDevices = decodeUint32(&frame.data[2]);
            if (graph.descriptor.transferId == 0 || graph.descriptor.expectedDevices == 0 ||
                frame.data[6] != 0 || frame.data[7] != 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphChecksum:
            graph.descriptor.transferId = decodeUint16(frame.data);
            graph.descriptor.checksum = decodeUint32(&frame.data[2]);
            if (graph.descriptor.transferId == 0 || frame.data[6] != 0 ||
                frame.data[7] != 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphChunk:
            graph.transferId = decodeUint16(frame.data);
            graph.sequence = decodeUint16(&frame.data[2]);
            graph.chunkSize = static_cast<uint8_t>(frame.length - 4);
            if (graph.transferId == 0 || graph.sequence >= kGraphChunkSequenceCapacity) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            std::memcpy(graph.chunk, &frame.data[4], graph.chunkSize);
            break;
        case MessageType::GraphFinish:
            graph.transferId = decodeUint16(frame.data);
            graph.sequence = decodeUint16(&frame.data[2]);
            if (graph.transferId == 0 || graph.sequence == 0 ||
                graph.sequence > kGraphChunkSequenceCapacity) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphAbort:
            graph.transferId = decodeUint16(frame.data);
            if (graph.transferId == 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphActivate:
        case MessageType::GraphRollback:
            graph.generation = decodeUint32(frame.data);
            graph.checksum = decodeUint32(&frame.data[4]);
            if (graph.generation == 0) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphStatusRequest:
            graph.transferId = decodeUint16(frame.data);
            break;
        case MessageType::GraphStatusIdentity:
            graph.transferId = decodeUint16(frame.data);
            graph.generation = decodeUint32(&frame.data[2]);
            graph.state = static_cast<GraphUpdateState>(frame.data[6]);
            graph.error = static_cast<GraphUpdateError>(frame.data[7]);
            if (!validGraphState(graph.state) || !validGraphError(graph.error)) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::GraphStatusProgress:
            graph.transferId = decodeUint16(frame.data);
            graph.checksum = decodeUint32(&frame.data[2]);
            graph.sequence = decodeUint16(&frame.data[6]);
            if (graph.sequence > kGraphChunkSequenceCapacity) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        case MessageType::HealthCheck:
        case MessageType::Reset:
        case MessageType::ClearTriggers:
            break;
    }

    result.error = ProtocolError::None;
    return result;
}

}  // namespace piho
