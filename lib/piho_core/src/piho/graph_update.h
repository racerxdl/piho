#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/graph_store.h"
#include "piho/protocol.h"

namespace piho {

constexpr uint32_t kGraphUpdateRetryIntervalMs = 50;
constexpr uint32_t kGraphUpdateSessionTimeoutMs = 5000;
constexpr uint8_t kGraphAnnouncementPartCount = 4;

using GraphUpdateFrameWrite = bool (*)(void *context, const CanFrame &frame);
using GraphActivationApply = bool (*)(void *context,
                                      const GraphIdentity &identity,
                                      uint32_t nowMilliseconds);

struct GraphNodeUpdateStatus {
    uint16_t transferId = 0;
    uint16_t nextSequence = 0;
    uint32_t generation = 0;
    uint32_t checksum = 0;
    GraphUpdateState state = GraphUpdateState::Idle;
    GraphUpdateError error = GraphUpdateError::None;
};

struct GraphUpdateCounters {
    uint32_t acceptedChunks = 0;
    uint32_t duplicateChunks = 0;
    uint32_t rejectedMessages = 0;
    uint32_t transmittedFrames = 0;
    uint32_t sendFailures = 0;
    uint32_t timeouts = 0;
};

class GraphUpdateParticipant {
   public:
    bool begin(uint8_t device, GraphDeviceRole role, GraphStore &store,
               GraphUpdateFrameWrite writeFrame, void *writeContext,
               uint32_t nowMilliseconds,
               GraphActivationApply applyActive = nullptr,
               void *applyContext = nullptr);
    void handle(const ProtocolMessage &message, uint32_t nowMilliseconds);
    void service(uint32_t nowMilliseconds);

    const GraphNodeUpdateStatus &status() const { return status_; }
    const GraphUpdateCounters &counters() const { return counters_; }

   private:
    static bool timeReached(uint32_t now, uint32_t deadline);
    static bool generationIsNewer(uint32_t candidate, uint32_t reference);
    static bool descriptorsEqual(const GraphTransferDescriptor &left,
                                 const GraphTransferDescriptor &right);
    static void increment(uint32_t &counter);
    bool applyActiveGraph(uint32_t nowMilliseconds);

    void handleAnnouncement(const ProtocolMessage &message, uint32_t nowMilliseconds);
    void handleChunk(const GraphTransferMessage &graph, uint32_t nowMilliseconds);
    void handleFinish(const GraphTransferMessage &graph, uint32_t nowMilliseconds);
    void handleAbort(const GraphTransferMessage &graph, uint32_t nowMilliseconds);
    void handleActivate(const GraphTransferMessage &graph, uint32_t nowMilliseconds);
    void handleRollback(const GraphTransferMessage &graph, uint32_t nowMilliseconds);
    void completeAnnouncement(uint32_t nowMilliseconds);
    void reject(GraphUpdateError error, uint32_t nowMilliseconds, bool discardStaged);
    void publish();
    void publishProgress();
    void publishInventory();
    void publishTransient(uint16_t transferId, uint32_t generation, uint32_t checksum,
                          GraphUpdateError error);
    void resetAnnouncement(uint16_t transferId, uint32_t nowMilliseconds);
    void syncStatusFromStore();

    GraphStore *store_ = nullptr;
    GraphUpdateFrameWrite writeFrame_ = nullptr;
    void *writeContext_ = nullptr;
    GraphActivationApply applyActive_ = nullptr;
    void *applyContext_ = nullptr;
    GraphTransferDescriptor descriptor_{};
    GraphNodeUpdateStatus status_{};
    GraphUpdateCounters counters_{};
    uint32_t lastActivityAt_ = 0;
    uint8_t device_ = 0;
    GraphDeviceRole role_ = GraphDeviceRole::Input;
    uint8_t announcementParts_ = 0;
    uint8_t lastChunk_[kGraphChunkDataCapacity]{};
    uint8_t lastChunkSize_ = 0;
    uint16_t lastChunkSequence_ = 0;
    bool hasLastChunk_ = false;
    bool initialized_ = false;
};

struct GraphGatewayStatus {
    GraphTransferDescriptor descriptor{};
    GraphIdentity rollbackTarget{};
    GraphUpdateState state = GraphUpdateState::Idle;
    GraphUpdateError lastError = GraphUpdateError::None;
    uint32_t readyDevices = 0;
    uint32_t progressedDevices = 0;
    uint32_t stagedDevices = 0;
    uint32_t rejectedDevices = 0;
    uint32_t activeDevices = 0;
    uint32_t rollbackDevices = 0;
    uint32_t missingDevices = 0;
    uint16_t nextSequence = 0;
    uint16_t sequenceCount = 0;
    bool chunkPending = false;
};

class GraphUpdateCoordinator {
   public:
    void configure(GraphUpdateFrameWrite writeFrame, void *writeContext = nullptr);

    GraphUpdateError beginUpdate(const GraphTransferDescriptor &descriptor,
                                 uint32_t nowMilliseconds);
    GraphUpdateError queueChunk(uint16_t transferId, uint16_t sequence,
                                const uint8_t *data, uint8_t size,
                                uint32_t nowMilliseconds);
    GraphUpdateError finishUpdate(uint16_t transferId, uint16_t sequenceCount,
                                  uint32_t nowMilliseconds);
    GraphUpdateError activateUpdate(uint32_t nowMilliseconds);
    GraphUpdateError rollbackUpdate(const GraphIdentity &target, uint32_t expectedDevices,
                                    uint32_t nowMilliseconds);
    GraphUpdateError abortUpdate(uint16_t transferId, uint32_t nowMilliseconds);
    GraphUpdateError requestStatus(uint16_t transferId, uint32_t nowMilliseconds);

    void handle(const ProtocolMessage &message, uint32_t nowMilliseconds);
    void service(uint32_t nowMilliseconds);

    const GraphGatewayStatus &status() const { return status_; }
    const GraphUpdateCounters &counters() const { return counters_; }
    uint32_t revision() const { return revision_; }

   private:
    enum class Phase : uint8_t {
        Idle,
        Announcing,
        Receiving,
        Finishing,
        Staged,
        Activating,
        Active,
        RollingBack,
        Rejected,
    };

    struct NodeObservation {
        uint16_t transferId = 0;
        uint16_t nextSequence = 0;
        uint32_t generation = 0;
        uint32_t checksum = 0;
        GraphUpdateState state = GraphUpdateState::Idle;
        GraphUpdateError error = GraphUpdateError::None;
        bool hasIdentity = false;
        bool hasProgress = false;
    };

    static bool timeReached(uint32_t now, uint32_t deadline);
    static bool descriptorsValid(const GraphTransferDescriptor &descriptor);
    static bool descriptorsEqual(const GraphTransferDescriptor &left,
                                 const GraphTransferDescriptor &right);
    static bool identitiesEqual(const GraphIdentity &left, const GraphIdentity &right);
    static void increment(uint32_t &counter);

    bool sendAnnouncementPart(uint8_t part);
    bool sendPendingChunk();
    bool sendFinish();
    bool sendActivation();
    bool sendRollback();
    bool sendAbort();
    bool sendFrame(const CanFrame &frame);
    void recompute(uint32_t nowMilliseconds);
    void reject(GraphUpdateError error, uint32_t nowMilliseconds, bool sendAbortFrame);
    void resetObservations();
    void revise();

    GraphUpdateFrameWrite writeFrame_ = nullptr;
    void *writeContext_ = nullptr;
    GraphGatewayStatus status_{};
    GraphUpdateCounters counters_{};
    NodeObservation nodes_[kDeviceCount]{};
    GraphIdentity activationIdentity_{};
    uint8_t pendingChunk_[kGraphChunkDataCapacity]{};
    uint8_t pendingChunkSize_ = 0;
    uint8_t lastChunk_[kGraphChunkDataCapacity]{};
    uint8_t lastChunkSize_ = 0;
    uint8_t announcementPart_ = 0;
    uint16_t pendingSequence_ = 0;
    uint16_t lastChunkSequence_ = 0;
    uint32_t lastActivityAt_ = 0;
    uint32_t nextSendAt_ = 0;
    uint32_t revision_ = 0;
    Phase phase_ = Phase::Idle;
    bool hasLastChunk_ = false;
    bool configured_ = false;
};

}  // namespace piho
