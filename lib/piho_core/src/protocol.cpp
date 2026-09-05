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

uint16_t decodeUint16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

bool acceptsBroadcast(MessageType type) {
    return type == MessageType::HealthCheck || type == MessageType::Reset;
}

bool validDevice(MessageType type, uint8_t device) {
    return isPhysicalDevice(device) || (device == kBroadcastDevice && acceptsBroadcast(type));
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
    result.message.device = device;
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
        case MessageType::HealthCheck:
        case MessageType::Reset:
        case MessageType::ClearTriggers:
            break;
    }

    result.error = ProtocolError::None;
    return result;
}

}  // namespace piho
