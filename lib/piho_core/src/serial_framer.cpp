#include "piho/serial_framer.h"

namespace piho {
namespace {

uint16_t updateCrc16(uint16_t crc, uint8_t byte) {
    crc ^= static_cast<uint16_t>(byte) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000u) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

}  // namespace

FrameParseStatus SerialFrameParser::push(uint8_t byte, SerialFrameView &frame) {
    frame = SerialFrameView{};
    switch (state_) {
        case State::MagicFirst:
            if (byte == kSerialMagicFirst) {
                state_ = State::MagicSecond;
            }
            return FrameParseStatus::None;
        case State::MagicSecond:
            if (byte == kSerialMagicSecond) {
                state_ = State::Version;
            } else {
                resetWithCandidate(byte);
            }
            return FrameParseStatus::None;
        case State::Version:
            if (byte != kSerialProtocolVersion) {
                resetWithCandidate(byte);
                return FrameParseStatus::UnsupportedVersion;
            }
            checksum_ = updateCrc16(0xFFFF, byte);
            state_ = State::LengthLow;
            return FrameParseStatus::None;
        case State::LengthLow:
            payloadLength_ = byte;
            checksum_ = updateCrc16(checksum_, byte);
            state_ = State::LengthHigh;
            return FrameParseStatus::None;
        case State::LengthHigh:
            payloadLength_ = static_cast<uint16_t>(payloadLength_ | static_cast<uint16_t>(byte) << 8);
            checksum_ = updateCrc16(checksum_, byte);
            payloadPosition_ = 0;
            if (payloadLength_ > kSerialPayloadCapacity) {
                resetWithCandidate(byte);
                return FrameParseStatus::InvalidLength;
            }
            state_ = payloadLength_ == 0 ? State::ChecksumLow : State::Payload;
            return FrameParseStatus::None;
        case State::Payload:
            payload_[payloadPosition_++] = byte;
            checksum_ = updateCrc16(checksum_, byte);
            if (payloadPosition_ == payloadLength_) {
                state_ = State::ChecksumLow;
            }
            return FrameParseStatus::None;
        case State::ChecksumLow:
            expectedChecksum_ = byte;
            state_ = State::ChecksumHigh;
            return FrameParseStatus::None;
        case State::ChecksumHigh: {
            expectedChecksum_ = static_cast<uint16_t>(expectedChecksum_ | static_cast<uint16_t>(byte) << 8);
            const bool valid = expectedChecksum_ == checksum_;
            if (valid) {
                frame.data = payload_;
                frame.length = payloadLength_;
            }
            resetWithCandidate(byte);
            return valid ? FrameParseStatus::Complete : FrameParseStatus::InvalidChecksum;
        }
    }
    reset();
    return FrameParseStatus::None;
}

void SerialFrameParser::reset() {
    state_ = State::MagicFirst;
    payloadLength_ = 0;
    payloadPosition_ = 0;
    expectedChecksum_ = 0;
    checksum_ = 0xFFFF;
}

void SerialFrameParser::resetWithCandidate(uint8_t byte) {
    reset();
    if (byte == kSerialMagicFirst) {
        state_ = State::MagicSecond;
    }
}

bool SerialFrameEncoder::encode(const uint8_t *payload, uint16_t payloadLength, uint8_t *output,
                                std::size_t outputCapacity, std::size_t &outputLength) {
    outputLength = 0;
    if (payloadLength > kSerialPayloadCapacity || output == nullptr ||
        (payloadLength != 0 && payload == nullptr)) {
        return false;
    }

    const std::size_t required = static_cast<std::size_t>(payloadLength) + kSerialFrameOverhead;
    if (outputCapacity < required) {
        return false;
    }

    output[0] = kSerialMagicFirst;
    output[1] = kSerialMagicSecond;
    output[2] = kSerialProtocolVersion;
    output[3] = static_cast<uint8_t>(payloadLength & 0xFFu);
    output[4] = static_cast<uint8_t>((payloadLength >> 8) & 0xFFu);

    uint16_t checksum = 0xFFFF;
    checksum = updateCrc16(checksum, output[2]);
    checksum = updateCrc16(checksum, output[3]);
    checksum = updateCrc16(checksum, output[4]);
    for (uint16_t index = 0; index < payloadLength; ++index) {
        output[5 + index] = payload[index];
        checksum = updateCrc16(checksum, payload[index]);
    }
    output[5 + payloadLength] = static_cast<uint8_t>(checksum & 0xFFu);
    output[6 + payloadLength] = static_cast<uint8_t>((checksum >> 8) & 0xFFu);
    outputLength = required;
    return true;
}

}  // namespace piho
