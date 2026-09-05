#pragma once

#include <cstdint>

#include "piho/can_frame.h"
#include "piho/trigger_table.h"

namespace piho {

constexpr uint8_t kBroadcastDevice = 0xFF;
constexpr uint16_t kProtocolNamespace = 0x150;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint32_t kProtocolHeader = (static_cast<uint32_t>(kProtocolNamespace) << 20) |
                                     (static_cast<uint32_t>(kProtocolVersion) << 16);
constexpr uint32_t kProtocolHeaderMask = 0x1FFF0000u;

enum class MessageType : uint8_t {
    HealthCheck = 1,
    Reset = 2,
    InputState = 3,
    OutputState = 4,
    SetPin = 5,
    SetByte = 6,
    UpsertTrigger = 7,
    RemoveTrigger = 8,
    ClearTriggers = 9,
};

enum class ProtocolError : uint8_t {
    None,
    NotExtended,
    RemoteFrame,
    InvalidIdentifier,
    UnsupportedType,
    InvalidDevice,
    InvalidLength,
    InvalidPayload,
};

struct ProtocolMessage {
    MessageType type = MessageType::HealthCheck;
    uint8_t device = 0;
    uint16_t state = 0;
    uint8_t index = 0;
    uint8_t value = 0;
    TriggerRule trigger{};
};

struct DecodeResult {
    ProtocolError error = ProtocolError::InvalidIdentifier;
    ProtocolMessage message{};

    bool ok() const { return error == ProtocolError::None; }
};

class ProtocolCodec {
   public:
    static CanFrame healthCheck(uint8_t device = kBroadcastDevice);
    static CanFrame reset(uint8_t device = kBroadcastDevice);
    static bool inputState(uint8_t sourceDevice, uint16_t state, CanFrame &frame);
    static bool outputState(uint8_t targetDevice, uint16_t state, CanFrame &frame);
    static bool setPin(uint8_t targetDevice, uint8_t localPin, bool value, CanFrame &frame);
    static bool setByte(uint8_t targetDevice, uint8_t localByte, uint8_t value, CanFrame &frame);
    static bool upsertTrigger(uint8_t targetDevice, const TriggerRule &rule, CanFrame &frame);
    static bool removeTrigger(uint8_t targetDevice, const TriggerRule &rule, CanFrame &frame);
    static bool clearTriggers(uint8_t targetDevice, CanFrame &frame);

    static DecodeResult decode(const CanFrame &frame);

   private:
    static CanFrame makeFrame(MessageType type, uint8_t device);
};

}  // namespace piho
