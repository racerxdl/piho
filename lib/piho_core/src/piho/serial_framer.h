#pragma once

#include <cstddef>
#include <cstdint>

namespace piho {

constexpr uint8_t kSerialMagicFirst = 'P';
constexpr uint8_t kSerialMagicSecond = 'H';
constexpr uint8_t kSerialProtocolVersion = 1;
constexpr std::size_t kSerialPayloadCapacity = 128;
constexpr std::size_t kSerialFrameOverhead = 7;
constexpr std::size_t kSerialFrameCapacity = kSerialPayloadCapacity + kSerialFrameOverhead;

enum class FrameParseStatus : uint8_t {
    None,
    Complete,
    UnsupportedVersion,
    InvalidLength,
    InvalidChecksum,
};

struct SerialFrameView {
    const uint8_t *data = nullptr;
    uint16_t length = 0;
};

class SerialFrameParser {
   public:
    FrameParseStatus push(uint8_t byte, SerialFrameView &frame);
    void reset();

   private:
    enum class State : uint8_t {
        MagicFirst,
        MagicSecond,
        Version,
        LengthLow,
        LengthHigh,
        Payload,
        ChecksumLow,
        ChecksumHigh,
    };

    void resetWithCandidate(uint8_t byte);

    State state_ = State::MagicFirst;
    uint8_t payload_[kSerialPayloadCapacity]{};
    uint16_t payloadLength_ = 0;
    uint16_t payloadPosition_ = 0;
    uint16_t expectedChecksum_ = 0;
    uint16_t checksum_ = 0xFFFF;
};

class SerialFrameEncoder {
   public:
    static bool encode(const uint8_t *payload, uint16_t payloadLength, uint8_t *output,
                       std::size_t outputCapacity, std::size_t &outputLength);
};

}  // namespace piho
