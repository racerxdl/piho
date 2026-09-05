#pragma once

#include <cstddef>
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
constexpr std::size_t kGraphChunkDataCapacity = 4;
constexpr uint16_t kGraphChunkSequenceCapacity =
    static_cast<uint16_t>((kGraphImageCapacity + kGraphChunkDataCapacity - 1) /
                          kGraphChunkDataCapacity);

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
    GraphBegin = 16,
    GraphCompatibility = 17,
    GraphDevices = 18,
    GraphChecksum = 19,
    GraphChunk = 20,
    GraphFinish = 21,
    GraphAbort = 22,
    GraphActivate = 23,
    GraphRollback = 24,
    GraphStatusRequest = 25,
    GraphStatusIdentity = 26,
    GraphStatusProgress = 27,
    GraphNodeCapabilities = 28,
    GraphNodeState = 29,
    GraphActiveIdentity = 30,
    GraphStagedIdentity = 31,
    GraphRollbackIdentity = 32,
    GraphManifestStatus = 33,
    GraphRollbackManifest = 34,
    GraphTransportDrops = 35,
    GraphTransportErrors = 36,
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

enum class GraphUpdateState : uint8_t {
    Idle = 0,
    Receiving = 1,
    Validating = 2,
    Staged = 3,
    Active = 4,
    Rollback = 5,
    Rejected = 6,
};

enum class GraphUpdateError : uint8_t {
    None = 0,
    Conflict = 1,
    InvalidDescriptor = 2,
    StaleGeneration = 3,
    Storage = 4,
    OutOfOrder = 5,
    ConflictingChunk = 6,
    MissingChunk = 7,
    InvalidImage = 8,
    Timeout = 9,
    NotStaged = 10,
    NotReady = 11,
    SendFailed = 12,
    Aborted = 13,
    WrongRole = 14,
    Incompatible = 15,
};

struct GraphTransferDescriptor {
    uint16_t transferId = 0;
    uint16_t format = 0;
    uint16_t executorApi = 0;
    uint32_t generation = 0;
    uint32_t imageSize = 0;
    uint32_t checksum = 0;
    uint32_t expectedDevices = 0;
};

struct GraphTransferMessage {
    GraphTransferDescriptor descriptor{};
    uint16_t transferId = 0;
    uint16_t sequence = 0;
    uint32_t generation = 0;
    uint32_t checksum = 0;
    GraphUpdateState state = GraphUpdateState::Idle;
    GraphUpdateError error = GraphUpdateError::None;
    uint8_t chunkSize = 0;
    uint8_t chunk[kGraphChunkDataCapacity]{};
};

struct GraphNodeStatusMessage {
    GraphIdentity identity{};
    uint16_t transferId = 0;
    uint16_t format = 0;
    uint16_t executorApi = 0;
    uint32_t activeDevices = 0;
    uint32_t stagedDevices = 0;
    uint32_t rollbackDevices = 0;
    GraphDeviceRole role = GraphDeviceRole::Input;
    GraphUpdateState updateState = GraphUpdateState::Idle;
    GraphUpdateError updateError = GraphUpdateError::None;
    uint8_t storeState = 0;
    uint32_t rxDropped = 0;
    uint32_t txDropped = 0;
    uint32_t busErrors = 0;
    uint8_t storeError = 0;
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
    GraphTransferMessage graph{};
    GraphNodeStatusMessage graphNode{};
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
    static bool graphBegin(const GraphTransferDescriptor &descriptor, CanFrame &frame);
    static bool graphCompatibility(const GraphTransferDescriptor &descriptor, CanFrame &frame);
    static bool graphDevices(const GraphTransferDescriptor &descriptor, CanFrame &frame);
    static bool graphChecksum(const GraphTransferDescriptor &descriptor, CanFrame &frame);
    static bool graphChunk(uint16_t transferId, uint16_t sequence, const uint8_t *data,
                           uint8_t size, CanFrame &frame);
    static bool graphFinish(uint16_t transferId, uint16_t sequenceCount, CanFrame &frame);
    static bool graphAbort(uint16_t transferId, CanFrame &frame);
    static bool graphActivate(const GraphIdentity &identity, CanFrame &frame);
    static bool graphRollback(const GraphIdentity &identity, CanFrame &frame);
    static bool graphStatusRequest(uint16_t transferId, CanFrame &frame);
    static bool graphStatusIdentity(uint8_t sourceDevice, uint16_t transferId,
                                    uint32_t generation, GraphUpdateState state,
                                    GraphUpdateError error, CanFrame &frame);
    static bool graphStatusProgress(uint8_t sourceDevice, uint16_t transferId,
                                    uint32_t checksum, uint16_t nextSequence,
                                    CanFrame &frame);
    static bool graphNodeCapabilities(uint8_t sourceDevice, GraphDeviceRole role,
                                      uint16_t format, uint16_t executorApi,
                                      CanFrame &frame);
    static bool graphNodeState(uint8_t sourceDevice, uint16_t transferId,
                               GraphUpdateState updateState, GraphUpdateError updateError,
                               uint8_t storeState, uint8_t storeError, CanFrame &frame);
    static bool graphActiveIdentity(uint8_t sourceDevice, const GraphIdentity &identity,
                                    CanFrame &frame);
    static bool graphStagedIdentity(uint8_t sourceDevice, const GraphIdentity &identity,
                                    CanFrame &frame);
    static bool graphRollbackIdentity(uint8_t sourceDevice, const GraphIdentity &identity,
                                      CanFrame &frame);
    static bool graphManifestStatus(uint8_t sourceDevice, uint32_t activeDevices,
                                    uint32_t stagedDevices, CanFrame &frame);
    static bool graphRollbackManifest(uint8_t sourceDevice, uint32_t rollbackDevices,
                                      CanFrame &frame);
    static bool graphTransportDrops(uint8_t sourceDevice, uint32_t rxDropped,
                                    uint32_t txDropped, CanFrame &frame);
    static bool graphTransportErrors(uint8_t sourceDevice, uint32_t busErrors,
                                     CanFrame &frame);

    static DecodeResult decode(const CanFrame &frame);

   private:
    static CanFrame makeFrame(MessageType type, uint8_t device);
};

}  // namespace piho
