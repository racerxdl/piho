#include "piho/protocol.h"

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

bool acceptsBroadcast(MessageType type) {
    return type == MessageType::HealthCheck || type == MessageType::Reset;
}

bool validDevice(MessageType type, uint8_t encodedDevice) {
    if (type == MessageType::ActionAck) {
        return isPhysicalDevice(static_cast<uint8_t>(encodedDevice & 0x1Fu));
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
            return 2;
        case MessageType::UpsertTrigger:
        case MessageType::RemoveTrigger:
            return 3;
        case MessageType::ExecuteAction:
        case MessageType::ActionAck:
            return 8;
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

bool ProtocolCodec::setByte(uint8_t targetDevice, uint8_t localByte, uint8_t value, CanFrame &frame) {
    if (!isPhysicalDevice(targetDevice) || localByte >= kBytesPerDevice) {
        return false;
    }
    frame = makeFrame(MessageType::SetByte, targetDevice);
    frame.length = 2;
    frame.data[0] = localByte;
    frame.data[1] = value;
    return true;
}

bool ProtocolCodec::upsertTrigger(uint8_t targetDevice, const TriggerRule &rule, CanFrame &frame) {
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

bool ProtocolCodec::removeTrigger(uint8_t targetDevice, const TriggerRule &rule, CanFrame &frame) {
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
        static_cast<uint32_t>(request.actionId - 1) |
        (request.eventToken << 9) |
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
    if (frame.length != expectedLength(type)) {
        result.error = ProtocolError::InvalidLength;
        return result;
    }

    result.message.type = type;
    result.message.device =
        type == MessageType::ActionAck ? static_cast<uint8_t>(device & 0x1Fu) : device;
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
                (packed & 0x80000000u) != 0 || !validAckStatus(acknowledgement.status)) {
                result.error = ProtocolError::InvalidPayload;
                return result;
            }
            break;
        }
        case MessageType::HealthCheck:
        case MessageType::Reset:
        case MessageType::ClearTriggers:
            break;
    }

    result.error = ProtocolError::None;
    return result;
}

}  // namespace piho
