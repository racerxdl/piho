#pragma once

#include <cstdint>

#include "piho/can_frame.h"
#include "piho/graph_image.h"
#include "piho/trigger_table.h"

namespace piho {

constexpr uint8_t kBroadcastDevice = 0xFF;
constexpr uint16_t kProtocolNamespace = 0x150;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint32_t kProtocolHeader = (static_cast<uint32_t>(kProtocolNamespace) << 20) |
                                     (static_cast<uint32_t>(kProtocolVersion) << 16);
constexpr uint32_t kProtocolHeaderMask = 0x1FFF0000u;
constexpr uint8_t kGraphTransferMessageTypeMinimum = 16;
constexpr uint32_t kActionEventTokenMaximum = (1u << 17) - 1;

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
    ExecuteAction = 10,
    ActionAck = 11,
};

static_assert(static_cast<uint8_t>(MessageType::ExecuteAction) <
              kGraphTransferMessageTypeMinimum);
static_assert(static_cast<uint8_t>(MessageType::ActionAck) <
              kGraphTransferMessageTypeMinimum);

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

enum class ActionAckStatus : uint8_t {
    Executed = 1,
    AlreadyExecuted = 2,
    WrongGeneration = 3,
    UnknownAction = 4,
    WrongTarget = 5,
    InvalidAction = 6,
    UnavailableOutput = 7,
};

struct ActionRequest {
    uint32_t generation = 0;
    uint32_t eventToken = 0;
    uint16_t actionId = 0;
    uint8_t sourceDevice = 0;
    uint8_t targetDevice = 0;
    bool sourceValue = false;
};

struct ActionAcknowledgement {
    uint32_t generation = 0;
    uint32_t eventToken = 0;
    uint16_t actionId = 0;
    uint8_t sourceDevice = 0;
    uint8_t outputDevice = 0;
    ActionAckStatus status = ActionAckStatus::InvalidAction;
};

struct ProtocolMessage {
    MessageType type = MessageType::HealthCheck;
    uint8_t device = 0;
    uint16_t state = 0;
    uint8_t index = 0;
    uint8_t value = 0;
    TriggerRule trigger{};
    ActionRequest actionRequest{};
    ActionAcknowledgement actionAcknowledgement{};
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
    static bool executeAction(const ActionRequest &request, CanFrame &frame);
    static bool actionAcknowledgement(const ActionAcknowledgement &acknowledgement,
                                      CanFrame &frame);

    static DecodeResult decode(const CanFrame &frame);

   private:
    static CanFrame makeFrame(MessageType type, uint8_t device);
};

}  // namespace piho
