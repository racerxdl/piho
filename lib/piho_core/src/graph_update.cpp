#include "piho/graph_update.h"

#include <cstring>

namespace piho {
namespace {

constexpr uint8_t kBeginPart = 1u << 0;
constexpr uint8_t kCompatibilityPart = 1u << 1;
constexpr uint8_t kDevicesPart = 1u << 2;
constexpr uint8_t kChecksumPart = 1u << 3;
constexpr uint8_t kAllAnnouncementParts =
    kBeginPart | kCompatibilityPart | kDevicesPart | kChecksumPart;

bool identityMatches(const GraphIdentity &identity, uint32_t generation, uint32_t checksum) {
    return identity.generation == generation && identity.checksum == checksum;
}

uint16_t sequenceCountFor(uint32_t imageSize) {
    return static_cast<uint16_t>((imageSize + kGraphChunkDataCapacity - 1) /
                                 kGraphChunkDataCapacity);
}

}  // namespace

bool GraphUpdateParticipant::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool GraphUpdateParticipant::generationIsNewer(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

bool GraphUpdateParticipant::descriptorsEqual(const GraphTransferDescriptor &left,
                                              const GraphTransferDescriptor &right) {
    return left.transferId == right.transferId && left.format == right.format &&
           left.executorApi == right.executorApi && left.generation == right.generation &&
           left.imageSize == right.imageSize && left.checksum == right.checksum &&
           left.expectedDevices == right.expectedDevices;
}

void GraphUpdateParticipant::increment(uint32_t &counter) {
    if (counter != UINT32_MAX) {
        ++counter;
    }
}

bool GraphUpdateParticipant::begin(uint8_t device, GraphDeviceRole role,
                                   GraphStore &store,
                                   GraphUpdateFrameWrite writeFrame,
                                   void *writeContext,
                                   uint32_t nowMilliseconds,
                                   GraphActivationApply applyActive,
                                   void *applyContext) {
    if (!isPhysicalDevice(device) ||
        (role != GraphDeviceRole::Input &&
         role != GraphDeviceRole::Output) ||
        writeFrame == nullptr) {
        return false;
    }
    device_ = device;
    role_ = role;
    store_ = &store;
    writeFrame_ = writeFrame;
    writeContext_ = writeContext;
    applyActive_ = applyActive;
    applyContext_ = applyContext;
    descriptor_ = GraphTransferDescriptor{};
    status_ = GraphNodeUpdateStatus{};
    counters_ = GraphUpdateCounters{};
    announcementParts_ = 0;
    hasLastChunk_ = false;
    lastChunkSize_ = 0;
    lastActivityAt_ = nowMilliseconds;
    initialized_ = true;
    syncStatusFromStore();
    if (store_->hasActiveGraph() && !applyActiveGraph(nowMilliseconds)) {
        status_.state = GraphUpdateState::Rejected;
        status_.error = GraphUpdateError::Runtime;
        return false;
    }
    return true;
}

bool GraphUpdateParticipant::applyActiveGraph(uint32_t nowMilliseconds) {
    if (store_ == nullptr || !store_->hasActiveGraph()) {
        return false;
    }
    return applyActive_ == nullptr ||
           applyActive_(applyContext_, store_->status().active,
                        nowMilliseconds);
}

void GraphUpdateParticipant::syncStatusFromStore() {
    if (store_ == nullptr) {
        return;
    }
    const GraphStoreStatus &stored = store_->status();
    status_.transferId = 0;
    status_.nextSequence = 0;
    status_.error = stored.lastError == GraphStoreError::None ? GraphUpdateError::None
                                                              : GraphUpdateError::Storage;
    if (store_->hasStagedGraph()) {
        status_.state = GraphUpdateState::Staged;
        status_.generation = stored.staged.generation;
        status_.checksum = stored.staged.checksum;
    } else if (store_->hasActiveGraph()) {
        status_.state = stored.state == GraphStoreState::Rollback
                            ? GraphUpdateState::Rollback
                            : GraphUpdateState::Active;
        status_.generation = stored.active.generation;
        status_.checksum = stored.active.checksum;
    } else {
        status_.state = GraphUpdateState::Idle;
        status_.generation = 0;
        status_.checksum = 0;
    }
}

void GraphUpdateParticipant::resetAnnouncement(uint16_t transferId,
                                               uint32_t nowMilliseconds) {
    descriptor_ = GraphTransferDescriptor{};
    descriptor_.transferId = transferId;
    announcementParts_ = 0;
    hasLastChunk_ = false;
    lastChunkSize_ = 0;
    lastActivityAt_ = nowMilliseconds;
}

void GraphUpdateParticipant::publish() {
    if (!initialized_) {
        return;
    }
    CanFrame identity{};
    CanFrame progress{};
    if (ProtocolCodec::graphStatusIdentity(device_, status_.transferId, status_.generation,
                                           status_.state, status_.error, identity)) {
        if (writeFrame_(writeContext_, identity)) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
    if (ProtocolCodec::graphStatusProgress(device_, status_.transferId, status_.checksum,
                                           status_.nextSequence, progress)) {
        if (writeFrame_(writeContext_, progress)) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
}

void GraphUpdateParticipant::publishProgress() {
    CanFrame progress{};
    if (initialized_ &&
        ProtocolCodec::graphStatusProgress(device_, status_.transferId, status_.checksum,
                                           status_.nextSequence, progress)) {
        if (writeFrame_(writeContext_, progress)) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
}

void GraphUpdateParticipant::publishInventory() {
    if (!initialized_ || store_ == nullptr) {
        return;
    }
    const GraphStoreStatus &stored = store_->status();
    CanFrame frames[7]{};
    const bool encoded[] = {
        ProtocolCodec::graphNodeCapabilities(device_, role_, kGraphFormatVersion,
                                             kGraphExecutorApiVersion, frames[0]),
        ProtocolCodec::graphNodeState(
            device_, status_.transferId, status_.state, status_.error,
            static_cast<uint8_t>(stored.state), static_cast<uint8_t>(stored.lastError),
            frames[1]),
        ProtocolCodec::graphActiveIdentity(device_, stored.active, frames[2]),
        ProtocolCodec::graphStagedIdentity(device_, stored.staged, frames[3]),
        ProtocolCodec::graphRollbackIdentity(device_, stored.rollback, frames[4]),
        ProtocolCodec::graphManifestStatus(device_, stored.activeDevices,
                                           stored.stagedDevices, frames[5]),
        ProtocolCodec::graphRollbackManifest(device_, stored.rollbackDevices, frames[6]),
    };
    for (uint8_t index = 0; index < 7; ++index) {
        if (!encoded[index]) {
            continue;
        }
        if (writeFrame_(writeContext_, frames[index])) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
}

void GraphUpdateParticipant::publishTransient(uint16_t transferId, uint32_t generation,
                                              uint32_t checksum, GraphUpdateError error) {
    CanFrame identity{};
    CanFrame progress{};
    if (ProtocolCodec::graphStatusIdentity(device_, transferId, generation,
                                           GraphUpdateState::Rejected, error, identity)) {
        if (writeFrame_(writeContext_, identity)) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
    if (ProtocolCodec::graphStatusProgress(device_, transferId, checksum, 0, progress)) {
        if (writeFrame_(writeContext_, progress)) {
            increment(counters_.transmittedFrames);
        } else {
            increment(counters_.sendFailures);
        }
    }
    increment(counters_.rejectedMessages);
}

void GraphUpdateParticipant::reject(GraphUpdateError error, uint32_t nowMilliseconds,
                                    bool discardStaged) {
    if (store_ != nullptr && store_->status().state == GraphStoreState::Receiving) {
        if (store_->cancelReceive() != GraphStoreError::Interrupted) {
            error = GraphUpdateError::Storage;
        }
    }
    if (store_ != nullptr && discardStaged && store_->hasStagedGraph() &&
        store_->discardStaged() != GraphStoreError::None) {
        error = GraphUpdateError::Storage;
    }
    status_.transferId = descriptor_.transferId;
    status_.generation = descriptor_.generation;
    status_.checksum = descriptor_.checksum;
    status_.state = GraphUpdateState::Rejected;
    status_.error = error;
    status_.nextSequence = 0;
    announcementParts_ = 0;
    hasLastChunk_ = false;
    lastActivityAt_ = nowMilliseconds;
    increment(counters_.rejectedMessages);
    publish();
}

void GraphUpdateParticipant::handleAnnouncement(const ProtocolMessage &message,
                                                uint32_t nowMilliseconds) {
    const GraphTransferDescriptor &part = message.graph.descriptor;
    const uint16_t transferId = part.transferId;
    const bool updateLocked = announcementParts_ != 0 ||
                              status_.state == GraphUpdateState::Receiving ||
                              status_.state == GraphUpdateState::Validating ||
                              status_.state == GraphUpdateState::Staged;
    if (descriptor_.transferId != 0 && transferId != descriptor_.transferId && updateLocked) {
        publishTransient(transferId, part.generation, part.checksum, GraphUpdateError::Conflict);
        return;
    }
    if (descriptor_.transferId == 0 || transferId != descriptor_.transferId ||
        status_.state == GraphUpdateState::Rejected) {
        if (status_.state == GraphUpdateState::Rejected) {
            syncStatusFromStore();
        }
        resetAnnouncement(transferId, nowMilliseconds);
    } else if ((status_.state == GraphUpdateState::Active ||
                status_.state == GraphUpdateState::Rollback) &&
               transferId == status_.transferId) {
        publish();
        return;
    }

    uint8_t partMask = 0;
    bool conflict = false;
    switch (message.type) {
        case MessageType::GraphBegin:
            partMask = kBeginPart;
            conflict = (announcementParts_ & partMask) != 0 &&
                       (descriptor_.generation != part.generation ||
                        descriptor_.imageSize != part.imageSize);
            descriptor_.generation = part.generation;
            descriptor_.imageSize = part.imageSize;
            break;
        case MessageType::GraphCompatibility:
            partMask = kCompatibilityPart;
            conflict = (announcementParts_ & partMask) != 0 &&
                       (descriptor_.format != part.format ||
                        descriptor_.executorApi != part.executorApi);
            descriptor_.format = part.format;
            descriptor_.executorApi = part.executorApi;
            break;
        case MessageType::GraphDevices:
            partMask = kDevicesPart;
            conflict = (announcementParts_ & partMask) != 0 &&
                       descriptor_.expectedDevices != part.expectedDevices;
            descriptor_.expectedDevices = part.expectedDevices;
            break;
        case MessageType::GraphChecksum:
            partMask = kChecksumPart;
            conflict = (announcementParts_ & partMask) != 0 &&
                       descriptor_.checksum != part.checksum;
            descriptor_.checksum = part.checksum;
            break;
        default:
            return;
    }
    if (conflict) {
        reject(GraphUpdateError::Conflict, nowMilliseconds, true);
        return;
    }
    announcementParts_ |= partMask;
    lastActivityAt_ = nowMilliseconds;
    if (announcementParts_ == kAllAnnouncementParts) {
        completeAnnouncement(nowMilliseconds);
    }
}

void GraphUpdateParticipant::completeAnnouncement(uint32_t nowMilliseconds) {
    const uint32_t localDeviceBit = static_cast<uint32_t>(1u) << device_;
    if (descriptor_.format != kGraphFormatVersion ||
        descriptor_.executorApi != kGraphExecutorApiVersion) {
        reject(GraphUpdateError::Incompatible, nowMilliseconds, false);
        return;
    }
    if (descriptor_.imageSize < kGraphHeaderSize ||
        descriptor_.imageSize > kGraphImageCapacity || descriptor_.generation == 0 ||
        descriptor_.expectedDevices == 0) {
        reject(GraphUpdateError::InvalidDescriptor, nowMilliseconds, false);
        return;
    }
    if ((descriptor_.expectedDevices & localDeviceBit) == 0) {
        descriptor_ = GraphTransferDescriptor{};
        announcementParts_ = 0;
        syncStatusFromStore();
        return;
    }

    const GraphStoreStatus &stored = store_->status();
    if (status_.state == GraphUpdateState::Receiving &&
        stored.state == GraphStoreState::Receiving) {
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (store_->hasStagedGraph() &&
        identityMatches(stored.staged, descriptor_.generation, descriptor_.checksum) &&
        stored.staged.format == descriptor_.format &&
        stored.staged.executorApi == descriptor_.executorApi &&
        stored.stagedDevices == descriptor_.expectedDevices &&
        store_->stagedDeviceRoleMatches(device_, role_)) {
        status_.transferId = descriptor_.transferId;
        status_.generation = descriptor_.generation;
        status_.checksum = descriptor_.checksum;
        status_.state = GraphUpdateState::Staged;
        status_.error = GraphUpdateError::None;
        status_.nextSequence = sequenceCountFor(descriptor_.imageSize);
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (store_->hasActiveGraph() &&
        identityMatches(stored.active, descriptor_.generation, descriptor_.checksum)) {
        status_.transferId = descriptor_.transferId;
        status_.generation = descriptor_.generation;
        status_.checksum = descriptor_.checksum;
        status_.state = GraphUpdateState::Active;
        status_.error = GraphUpdateError::None;
        status_.nextSequence = sequenceCountFor(descriptor_.imageSize);
        announcementParts_ = 0;
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (store_->hasActiveGraph() &&
        !generationIsNewer(descriptor_.generation, stored.active.generation)) {
        reject(GraphUpdateError::StaleGeneration, nowMilliseconds, false);
        return;
    }

    const GraphStoreError storeError = store_->beginReceive(
        GraphReceiveDescriptor{descriptor_.imageSize, descriptor_.generation,
                               descriptor_.checksum});
    if (storeError != GraphStoreError::None) {
        reject(storeError == GraphStoreError::InvalidArgument
                   ? GraphUpdateError::StaleGeneration
                   : GraphUpdateError::Storage,
               nowMilliseconds, false);
        return;
    }
    status_.transferId = descriptor_.transferId;
    status_.generation = descriptor_.generation;
    status_.checksum = descriptor_.checksum;
    status_.state = GraphUpdateState::Receiving;
    status_.error = GraphUpdateError::None;
    status_.nextSequence = 0;
    hasLastChunk_ = false;
    lastActivityAt_ = nowMilliseconds;
    publish();
}

void GraphUpdateParticipant::handleChunk(const GraphTransferMessage &graph,
                                         uint32_t nowMilliseconds) {
    if (graph.transferId != descriptor_.transferId) {
        publishTransient(graph.transferId, 0, 0, GraphUpdateError::Conflict);
        return;
    }
    if (status_.state != GraphUpdateState::Receiving) {
        publishTransient(graph.transferId, descriptor_.generation, descriptor_.checksum,
                         GraphUpdateError::NotReady);
        return;
    }

    if (hasLastChunk_ && graph.sequence == lastChunkSequence_) {
        if (graph.chunkSize == lastChunkSize_ &&
            std::memcmp(graph.chunk, lastChunk_, graph.chunkSize) == 0) {
            increment(counters_.duplicateChunks);
            lastActivityAt_ = nowMilliseconds;
            publishProgress();
        } else {
            reject(GraphUpdateError::ConflictingChunk, nowMilliseconds, false);
        }
        return;
    }
    if (graph.sequence != status_.nextSequence) {
        reject(graph.sequence > status_.nextSequence ? GraphUpdateError::MissingChunk
                                                     : GraphUpdateError::OutOfOrder,
               nowMilliseconds, false);
        return;
    }

    const uint32_t offset = static_cast<uint32_t>(graph.sequence) * kGraphChunkDataCapacity;
    if (offset >= descriptor_.imageSize) {
        reject(GraphUpdateError::OutOfOrder, nowMilliseconds, false);
        return;
    }
    const uint32_t remaining = descriptor_.imageSize - offset;
    const uint8_t expectedSize = static_cast<uint8_t>(
        remaining < kGraphChunkDataCapacity ? remaining : kGraphChunkDataCapacity);
    if (graph.chunkSize != expectedSize) {
        reject(GraphUpdateError::InvalidDescriptor, nowMilliseconds, false);
        return;
    }
    if (store_->writeChunk(graph.chunk, graph.chunkSize) != GraphStoreError::None) {
        reject(GraphUpdateError::Storage, nowMilliseconds, false);
        return;
    }

    std::memcpy(lastChunk_, graph.chunk, graph.chunkSize);
    lastChunkSize_ = graph.chunkSize;
    lastChunkSequence_ = graph.sequence;
    hasLastChunk_ = true;
    ++status_.nextSequence;
    lastActivityAt_ = nowMilliseconds;
    increment(counters_.acceptedChunks);
    publishProgress();
}

void GraphUpdateParticipant::handleFinish(const GraphTransferMessage &graph,
                                          uint32_t nowMilliseconds) {
    if (graph.transferId != descriptor_.transferId) {
        publishTransient(graph.transferId, 0, 0, GraphUpdateError::Conflict);
        return;
    }
    const uint16_t expectedSequences = sequenceCountFor(descriptor_.imageSize);
    if (status_.state == GraphUpdateState::Staged && graph.sequence == expectedSequences) {
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (status_.state != GraphUpdateState::Receiving) {
        publishTransient(graph.transferId, descriptor_.generation, descriptor_.checksum,
                         GraphUpdateError::NotReady);
        return;
    }
    if (graph.sequence != expectedSequences || status_.nextSequence != expectedSequences) {
        reject(GraphUpdateError::MissingChunk, nowMilliseconds, false);
        return;
    }

    status_.state = GraphUpdateState::Validating;
    status_.error = GraphUpdateError::None;
    lastActivityAt_ = nowMilliseconds;
    publish();
    const GraphStoreError error = store_->finishReceive();
    if (error != GraphStoreError::None) {
        reject(error == GraphStoreError::InvalidChecksum ||
                       error == GraphStoreError::InvalidImage ||
                       error == GraphStoreError::InvalidLength
                   ? GraphUpdateError::InvalidImage
                   : GraphUpdateError::Storage,
               nowMilliseconds, false);
        return;
    }

    const GraphStoreStatus &stored = store_->status();
    if (!store_->hasStagedGraph() ||
        !identityMatches(stored.staged, descriptor_.generation, descriptor_.checksum) ||
        stored.staged.format != descriptor_.format ||
        stored.staged.executorApi != descriptor_.executorApi ||
        stored.stagedDevices != descriptor_.expectedDevices) {
        store_->discardStaged();
        reject(GraphUpdateError::InvalidImage, nowMilliseconds, false);
        return;
    }
    if (!store_->stagedDeviceRoleMatches(device_, role_)) {
        store_->discardStaged();
        reject(GraphUpdateError::WrongRole, nowMilliseconds, false);
        return;
    }
    status_.state = GraphUpdateState::Staged;
    status_.error = GraphUpdateError::None;
    status_.generation = descriptor_.generation;
    status_.checksum = descriptor_.checksum;
    publish();
}

void GraphUpdateParticipant::handleAbort(const GraphTransferMessage &graph,
                                         uint32_t nowMilliseconds) {
    if (graph.transferId != descriptor_.transferId) {
        return;
    }
    if (status_.state == GraphUpdateState::Rejected &&
        status_.error == GraphUpdateError::Aborted) {
        publish();
        return;
    }
    reject(GraphUpdateError::Aborted, nowMilliseconds, true);
}

void GraphUpdateParticipant::handleActivate(const GraphTransferMessage &graph,
                                            uint32_t nowMilliseconds) {
    const GraphStoreStatus &stored = store_->status();
    if (store_->hasActiveGraph() &&
        identityMatches(stored.active, graph.generation, graph.checksum)) {
        if (!applyActiveGraph(nowMilliseconds)) {
            publishTransient(status_.transferId, graph.generation,
                             graph.checksum, GraphUpdateError::Runtime);
            return;
        }
        status_.generation = graph.generation;
        status_.checksum = graph.checksum;
        status_.state = GraphUpdateState::Active;
        status_.error = GraphUpdateError::None;
        status_.nextSequence = sequenceCountFor(descriptor_.imageSize);
        announcementParts_ = 0;
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (!store_->hasStagedGraph() ||
        !identityMatches(stored.staged, graph.generation, graph.checksum)) {
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::NotStaged);
        return;
    }
    if (store_->activate() != GraphStoreError::None) {
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::Storage);
        return;
    }
    if (!applyActiveGraph(nowMilliseconds)) {
        if (store_->hasRollbackGraph() &&
            store_->rollback() == GraphStoreError::None) {
            applyActiveGraph(nowMilliseconds);
        }
        syncStatusFromStore();
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::Runtime);
        return;
    }
    status_.generation = graph.generation;
    status_.checksum = graph.checksum;
    status_.state = GraphUpdateState::Active;
    status_.error = GraphUpdateError::None;
    announcementParts_ = 0;
    lastActivityAt_ = nowMilliseconds;
    publish();
}

void GraphUpdateParticipant::handleRollback(const GraphTransferMessage &graph,
                                            uint32_t nowMilliseconds) {
    const GraphStoreStatus &stored = store_->status();
    if (store_->hasActiveGraph() &&
        identityMatches(stored.active, graph.generation, graph.checksum)) {
        if (!applyActiveGraph(nowMilliseconds)) {
            publishTransient(status_.transferId, graph.generation,
                             graph.checksum, GraphUpdateError::Runtime);
            return;
        }
        status_.generation = graph.generation;
        status_.checksum = graph.checksum;
        status_.state = GraphUpdateState::Rollback;
        status_.error = GraphUpdateError::None;
        announcementParts_ = 0;
        lastActivityAt_ = nowMilliseconds;
        publish();
        return;
    }
    if (!store_->hasRollbackGraph() ||
        !identityMatches(stored.rollback, graph.generation, graph.checksum)) {
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::NotStaged);
        return;
    }
    if (store_->rollback() != GraphStoreError::None) {
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::Storage);
        return;
    }
    if (!applyActiveGraph(nowMilliseconds)) {
        if (store_->hasRollbackGraph() &&
            store_->rollback() == GraphStoreError::None) {
            applyActiveGraph(nowMilliseconds);
        }
        syncStatusFromStore();
        publishTransient(status_.transferId, graph.generation, graph.checksum,
                         GraphUpdateError::Runtime);
        return;
    }
    status_.generation = graph.generation;
    status_.checksum = graph.checksum;
    status_.state = GraphUpdateState::Rollback;
    status_.error = GraphUpdateError::None;
    announcementParts_ = 0;
    lastActivityAt_ = nowMilliseconds;
    publish();
}

void GraphUpdateParticipant::handle(const ProtocolMessage &message,
                                    uint32_t nowMilliseconds) {
    if (!initialized_) {
        return;
    }
    switch (message.type) {
        case MessageType::GraphBegin:
        case MessageType::GraphCompatibility:
        case MessageType::GraphDevices:
        case MessageType::GraphChecksum:
            handleAnnouncement(message, nowMilliseconds);
            break;
        case MessageType::GraphChunk:
            handleChunk(message.graph, nowMilliseconds);
            break;
        case MessageType::GraphFinish:
            handleFinish(message.graph, nowMilliseconds);
            break;
        case MessageType::GraphAbort:
            handleAbort(message.graph, nowMilliseconds);
            break;
        case MessageType::GraphActivate:
            handleActivate(message.graph, nowMilliseconds);
            break;
        case MessageType::GraphRollback:
            handleRollback(message.graph, nowMilliseconds);
            break;
        case MessageType::GraphStatusRequest:
            if (message.graph.transferId == 0 || status_.transferId == 0 ||
                message.graph.transferId == status_.transferId) {
                publish();
                publishInventory();
            }
            break;
        default:
            break;
    }
}

void GraphUpdateParticipant::service(uint32_t nowMilliseconds) {
    if (!initialized_ ||
        (announcementParts_ == 0 && status_.state != GraphUpdateState::Receiving &&
         status_.state != GraphUpdateState::Staged) ||
        !timeReached(nowMilliseconds, lastActivityAt_ + kGraphUpdateSessionTimeoutMs)) {
        return;
    }
    increment(counters_.timeouts);
    reject(GraphUpdateError::Timeout, nowMilliseconds, true);
}

bool GraphUpdateCoordinator::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool GraphUpdateCoordinator::descriptorsValid(const GraphTransferDescriptor &descriptor) {
    return descriptor.transferId != 0 && descriptor.format == kGraphFormatVersion &&
           descriptor.executorApi == kGraphExecutorApiVersion && descriptor.generation != 0 &&
           descriptor.imageSize >= kGraphHeaderSize &&
           descriptor.imageSize <= kGraphImageCapacity && descriptor.expectedDevices != 0;
}

bool GraphUpdateCoordinator::descriptorsEqual(const GraphTransferDescriptor &left,
                                              const GraphTransferDescriptor &right) {
    return left.transferId == right.transferId && left.format == right.format &&
           left.executorApi == right.executorApi && left.generation == right.generation &&
           left.imageSize == right.imageSize && left.checksum == right.checksum &&
           left.expectedDevices == right.expectedDevices;
}

bool GraphUpdateCoordinator::identitiesEqual(const GraphIdentity &left,
                                             const GraphIdentity &right) {
    return left.generation == right.generation && left.checksum == right.checksum;
}

void GraphUpdateCoordinator::increment(uint32_t &counter) {
    if (counter != UINT32_MAX) {
        ++counter;
    }
}

void GraphUpdateCoordinator::configure(GraphUpdateFrameWrite writeFrame,
                                       void *writeContext) {
    writeFrame_ = writeFrame;
    writeContext_ = writeContext;
    configured_ = writeFrame != nullptr;
}

void GraphUpdateCoordinator::resetObservations() {
    for (NodeObservation &node : nodes_) {
        node = NodeObservation{};
    }
}

void GraphUpdateCoordinator::revise() {
    ++revision_;
    if (revision_ == 0) {
        revision_ = 1;
    }
}

GraphUpdateError GraphUpdateCoordinator::beginUpdate(
    const GraphTransferDescriptor &descriptor, uint32_t nowMilliseconds) {
    if (!configured_ || !descriptorsValid(descriptor)) {
        return GraphUpdateError::InvalidDescriptor;
    }
    if (phase_ == Phase::Announcing ||
        (phase_ == Phase::Receiving && status_.nextSequence == 0 &&
         !status_.chunkPending)) {
        return descriptorsEqual(status_.descriptor, descriptor)
                   ? GraphUpdateError::None
                   : GraphUpdateError::Conflict;
    }
    if (phase_ == Phase::Receiving || phase_ == Phase::Finishing ||
        phase_ == Phase::Staged || phase_ == Phase::Activating ||
        phase_ == Phase::RollingBack) {
        return GraphUpdateError::Conflict;
    }

    status_ = GraphGatewayStatus{};
    status_.descriptor = descriptor;
    status_.state = GraphUpdateState::Receiving;
    status_.sequenceCount = sequenceCountFor(descriptor.imageSize);
    status_.missingDevices = descriptor.expectedDevices;
    activationIdentity_ = GraphIdentity{descriptor.format, descriptor.executorApi,
                                        descriptor.generation, descriptor.checksum};
    resetObservations();
    pendingChunkSize_ = 0;
    lastChunkSize_ = 0;
    announcementPart_ = 0;
    pendingSequence_ = 0;
    hasLastChunk_ = false;
    phase_ = Phase::Announcing;
    lastActivityAt_ = nowMilliseconds;
    nextSendAt_ = nowMilliseconds;
    revise();
    service(nowMilliseconds);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::queueChunk(uint16_t transferId, uint16_t sequence,
                                                     const uint8_t *data, uint8_t size,
                                                     uint32_t nowMilliseconds) {
    if (!configured_ || data == nullptr || size == 0 || size > kGraphChunkDataCapacity) {
        return GraphUpdateError::InvalidDescriptor;
    }
    if (transferId != status_.descriptor.transferId) {
        return GraphUpdateError::Conflict;
    }
    if (phase_ != Phase::Receiving ||
        status_.readyDevices != status_.descriptor.expectedDevices) {
        return GraphUpdateError::NotReady;
    }
    if (status_.chunkPending) {
        return sequence == pendingSequence_ && size == pendingChunkSize_ &&
                       std::memcmp(data, pendingChunk_, size) == 0
                   ? GraphUpdateError::None
                   : GraphUpdateError::Conflict;
    }
    if (sequence < status_.nextSequence) {
        return hasLastChunk_ && sequence == lastChunkSequence_ && size == lastChunkSize_ &&
                       std::memcmp(data, lastChunk_, size) == 0
                   ? GraphUpdateError::None
                   : GraphUpdateError::OutOfOrder;
    }
    if (sequence != status_.nextSequence) {
        return GraphUpdateError::OutOfOrder;
    }
    const uint32_t offset = static_cast<uint32_t>(sequence) * kGraphChunkDataCapacity;
    if (offset >= status_.descriptor.imageSize) {
        return GraphUpdateError::OutOfOrder;
    }
    const uint32_t remaining = status_.descriptor.imageSize - offset;
    const uint8_t expectedSize = static_cast<uint8_t>(
        remaining < kGraphChunkDataCapacity ? remaining : kGraphChunkDataCapacity);
    if (size != expectedSize) {
        return GraphUpdateError::InvalidDescriptor;
    }

    std::memcpy(pendingChunk_, data, size);
    pendingChunkSize_ = size;
    pendingSequence_ = sequence;
    status_.chunkPending = true;
    status_.progressedDevices = 0;
    lastActivityAt_ = nowMilliseconds;
    nextSendAt_ = nowMilliseconds;
    revise();
    service(nowMilliseconds);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::finishUpdate(uint16_t transferId,
                                                      uint16_t sequenceCount,
                                                      uint32_t nowMilliseconds) {
    if (transferId != status_.descriptor.transferId) {
        return GraphUpdateError::Conflict;
    }
    if ((phase_ == Phase::Finishing || phase_ == Phase::Staged ||
         phase_ == Phase::Activating || phase_ == Phase::Active) &&
        sequenceCount == status_.sequenceCount) {
        return GraphUpdateError::None;
    }
    if (phase_ != Phase::Receiving || status_.chunkPending ||
        status_.readyDevices != status_.descriptor.expectedDevices) {
        return GraphUpdateError::NotReady;
    }
    if (sequenceCount != status_.sequenceCount || status_.nextSequence != sequenceCount) {
        return GraphUpdateError::MissingChunk;
    }
    phase_ = Phase::Finishing;
    status_.state = GraphUpdateState::Validating;
    status_.missingDevices = status_.descriptor.expectedDevices;
    lastActivityAt_ = nowMilliseconds;
    nextSendAt_ = nowMilliseconds;
    revise();
    service(nowMilliseconds);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::activateUpdate(uint32_t nowMilliseconds) {
    if (phase_ == Phase::Activating ||
        (phase_ == Phase::Active &&
         status_.activeDevices == status_.descriptor.expectedDevices)) {
        return GraphUpdateError::None;
    }
    if (phase_ != Phase::Staged ||
        status_.stagedDevices != status_.descriptor.expectedDevices ||
        status_.rejectedDevices != 0) {
        return GraphUpdateError::NotReady;
    }
    phase_ = Phase::Activating;
    status_.state = GraphUpdateState::Staged;
    status_.missingDevices = status_.descriptor.expectedDevices;
    lastActivityAt_ = nowMilliseconds;
    nextSendAt_ = nowMilliseconds;
    revise();
    service(nowMilliseconds);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::rollbackUpdate(const GraphIdentity &target,
                                                        uint32_t expectedDevices,
                                                        uint32_t nowMilliseconds) {
    if (!configured_ || target.generation == 0 || expectedDevices == 0) {
        return GraphUpdateError::InvalidDescriptor;
    }
    if ((phase_ == Phase::RollingBack ||
         (phase_ == Phase::Active && status_.rollbackDevices == expectedDevices)) &&
        status_.descriptor.expectedDevices == expectedDevices &&
        identitiesEqual(status_.rollbackTarget, target)) {
        return GraphUpdateError::None;
    }
    if (phase_ == Phase::Announcing || phase_ == Phase::Receiving ||
        phase_ == Phase::Finishing || phase_ == Phase::Staged ||
        phase_ == Phase::Activating || phase_ == Phase::RollingBack) {
        return GraphUpdateError::Conflict;
    }
    if (phase_ == Phase::Active && status_.descriptor.expectedDevices != 0 &&
        status_.descriptor.expectedDevices != expectedDevices) {
        return GraphUpdateError::Conflict;
    }

    status_ = GraphGatewayStatus{};
    status_.descriptor.expectedDevices = expectedDevices;
    status_.rollbackTarget = target;
    status_.state = GraphUpdateState::Active;
    status_.missingDevices = expectedDevices;
    resetObservations();
    phase_ = Phase::RollingBack;
    lastActivityAt_ = nowMilliseconds;
    nextSendAt_ = nowMilliseconds;
    revise();
    service(nowMilliseconds);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::abortUpdate(uint16_t transferId,
                                                     uint32_t nowMilliseconds) {
    if (transferId == 0) {
        return GraphUpdateError::InvalidDescriptor;
    }
    if (transferId != status_.descriptor.transferId) {
        if (phase_ != Phase::Idle && phase_ != Phase::Rejected &&
            phase_ != Phase::Active) {
            return GraphUpdateError::Conflict;
        }
        CanFrame frame{};
        return ProtocolCodec::graphAbort(transferId, frame) && sendFrame(frame)
                   ? GraphUpdateError::None
                   : GraphUpdateError::SendFailed;
    }
    reject(GraphUpdateError::Aborted, nowMilliseconds, true);
    return GraphUpdateError::None;
}

GraphUpdateError GraphUpdateCoordinator::requestStatus(uint16_t transferId,
                                                       uint32_t nowMilliseconds) {
    if (!configured_ ||
        (transferId != 0 && transferId != status_.descriptor.transferId)) {
        return GraphUpdateError::Conflict;
    }
    CanFrame frame{};
    if (!ProtocolCodec::graphStatusRequest(transferId, frame)) {
        return GraphUpdateError::InvalidDescriptor;
    }
    lastActivityAt_ = nowMilliseconds;
    return sendFrame(frame) ? GraphUpdateError::None : GraphUpdateError::SendFailed;
}

bool GraphUpdateCoordinator::sendFrame(const CanFrame &frame) {
    if (writeFrame_ != nullptr && writeFrame_(writeContext_, frame)) {
        increment(counters_.transmittedFrames);
        return true;
    }
    increment(counters_.sendFailures);
    status_.lastError = GraphUpdateError::SendFailed;
    revise();
    return false;
}

bool GraphUpdateCoordinator::sendAnnouncementPart(uint8_t part) {
    CanFrame frame{};
    bool encoded = false;
    switch (part) {
        case 0:
            encoded = ProtocolCodec::graphBegin(status_.descriptor, frame);
            break;
        case 1:
            encoded = ProtocolCodec::graphCompatibility(status_.descriptor, frame);
            break;
        case 2:
            encoded = ProtocolCodec::graphDevices(status_.descriptor, frame);
            break;
        case 3:
            encoded = ProtocolCodec::graphChecksum(status_.descriptor, frame);
            break;
        default:
            return false;
    }
    return encoded && sendFrame(frame);
}

bool GraphUpdateCoordinator::sendPendingChunk() {
    CanFrame frame{};
    return ProtocolCodec::graphChunk(status_.descriptor.transferId, pendingSequence_,
                                     pendingChunk_, pendingChunkSize_, frame) &&
           sendFrame(frame);
}

bool GraphUpdateCoordinator::sendFinish() {
    CanFrame frame{};
    return ProtocolCodec::graphFinish(status_.descriptor.transferId, status_.sequenceCount,
                                      frame) &&
           sendFrame(frame);
}

bool GraphUpdateCoordinator::sendActivation() {
    CanFrame frame{};
    return ProtocolCodec::graphActivate(activationIdentity_, frame) && sendFrame(frame);
}

bool GraphUpdateCoordinator::sendRollback() {
    CanFrame frame{};
    return ProtocolCodec::graphRollback(status_.rollbackTarget, frame) && sendFrame(frame);
}

bool GraphUpdateCoordinator::sendAbort() {
    CanFrame frame{};
    return status_.descriptor.transferId != 0 &&
           ProtocolCodec::graphAbort(status_.descriptor.transferId, frame) && sendFrame(frame);
}

void GraphUpdateCoordinator::reject(GraphUpdateError error, uint32_t nowMilliseconds,
                                    bool sendAbortFrame) {
    if (sendAbortFrame) {
        sendAbort();
    }
    phase_ = Phase::Rejected;
    status_.state = GraphUpdateState::Rejected;
    status_.lastError = error;
    status_.chunkPending = false;
    status_.progressedDevices = 0;
    status_.missingDevices = status_.descriptor.expectedDevices &
                             ~(status_.stagedDevices | status_.activeDevices |
                               status_.rollbackDevices | status_.rejectedDevices);
    lastActivityAt_ = nowMilliseconds;
    increment(counters_.rejectedMessages);
    revise();
}

void GraphUpdateCoordinator::recompute(uint32_t nowMilliseconds) {
    uint32_t ready = 0;
    uint32_t progressed = 0;
    uint32_t staged = 0;
    uint32_t rejected = 0;
    uint32_t active = 0;
    uint32_t rolledBack = 0;
    for (uint8_t device = 0; device < kDeviceCount; ++device) {
        const uint32_t bit = static_cast<uint32_t>(1u) << device;
        if ((status_.descriptor.expectedDevices & bit) == 0) {
            continue;
        }
        const NodeObservation &node = nodes_[device];
        const bool persistedIdentity =
            node.transferId == 0 &&
            (node.state == GraphUpdateState::Staged ||
             node.state == GraphUpdateState::Active ||
             node.state == GraphUpdateState::Rollback);
        const bool rollbackObservation =
            phase_ == Phase::RollingBack && node.hasIdentity &&
            node.generation == status_.rollbackTarget.generation &&
            node.checksum == status_.rollbackTarget.checksum;
        if (node.transferId != status_.descriptor.transferId && !persistedIdentity &&
            !rollbackObservation) {
            continue;
        }
        if (node.hasIdentity && node.state == GraphUpdateState::Rejected) {
            rejected |= bit;
            continue;
        }
        const bool transferIdentity =
            node.hasIdentity && node.hasProgress &&
            node.generation == status_.descriptor.generation &&
            node.checksum == status_.descriptor.checksum;
        if (transferIdentity &&
            (node.state == GraphUpdateState::Receiving ||
             node.state == GraphUpdateState::Validating ||
             node.state == GraphUpdateState::Staged)) {
            ready |= bit;
        }
        if (transferIdentity && node.state == GraphUpdateState::Staged) {
            staged |= bit;
        }
        if (transferIdentity && node.state == GraphUpdateState::Active) {
            active |= bit;
        }
        if (status_.chunkPending && transferIdentity &&
            node.state == GraphUpdateState::Receiving &&
            node.nextSequence == static_cast<uint16_t>(pendingSequence_ + 1)) {
            progressed |= bit;
        }
        if (rollbackObservation && node.hasProgress &&
            node.state == GraphUpdateState::Rollback) {
            rolledBack |= bit;
        }
    }

    status_.readyDevices = ready;
    status_.progressedDevices = progressed;
    status_.stagedDevices = staged;
    status_.rejectedDevices = rejected;
    status_.activeDevices = active;
    status_.rollbackDevices = rolledBack;

    if (rejected != 0 && phase_ != Phase::Active && phase_ != Phase::Rejected) {
        GraphUpdateError error = GraphUpdateError::InvalidImage;
        for (uint8_t device = 0; device < kDeviceCount; ++device) {
            if ((rejected & (static_cast<uint32_t>(1u) << device)) != 0) {
                error = nodes_[device].error;
                break;
            }
        }
        reject(error, nowMilliseconds, true);
        return;
    }
    if (phase_ == Phase::Announcing && ready == status_.descriptor.expectedDevices) {
        phase_ = Phase::Receiving;
        status_.state = GraphUpdateState::Receiving;
        status_.lastError = GraphUpdateError::None;
        lastActivityAt_ = nowMilliseconds;
    }
    if (phase_ == Phase::Receiving && status_.chunkPending &&
        progressed == status_.descriptor.expectedDevices) {
        std::memcpy(lastChunk_, pendingChunk_, pendingChunkSize_);
        lastChunkSize_ = pendingChunkSize_;
        lastChunkSequence_ = pendingSequence_;
        hasLastChunk_ = true;
        status_.chunkPending = false;
        status_.progressedDevices = 0;
        ++status_.nextSequence;
        increment(counters_.acceptedChunks);
        lastActivityAt_ = nowMilliseconds;
    }
    if (phase_ == Phase::Finishing && staged == status_.descriptor.expectedDevices) {
        phase_ = Phase::Staged;
        status_.state = GraphUpdateState::Staged;
        status_.lastError = GraphUpdateError::None;
        lastActivityAt_ = nowMilliseconds;
    }
    if (phase_ == Phase::Activating && active == status_.descriptor.expectedDevices) {
        phase_ = Phase::Active;
        status_.state = GraphUpdateState::Active;
        status_.lastError = GraphUpdateError::None;
        status_.missingDevices = 0;
        lastActivityAt_ = nowMilliseconds;
    }
    if (phase_ == Phase::RollingBack &&
        rolledBack == status_.descriptor.expectedDevices) {
        phase_ = Phase::Active;
        status_.state = GraphUpdateState::Rollback;
        status_.lastError = GraphUpdateError::None;
        status_.missingDevices = 0;
        lastActivityAt_ = nowMilliseconds;
    }

    switch (phase_) {
        case Phase::Announcing:
            status_.missingDevices = status_.descriptor.expectedDevices & ~ready;
            break;
        case Phase::Receiving:
            status_.missingDevices = status_.chunkPending
                                         ? status_.descriptor.expectedDevices & ~progressed
                                         : status_.descriptor.expectedDevices & ~ready;
            break;
        case Phase::Finishing:
        case Phase::Staged:
            status_.missingDevices = status_.descriptor.expectedDevices & ~(staged | rejected);
            break;
        case Phase::Activating:
            status_.missingDevices = status_.descriptor.expectedDevices & ~(active | rejected);
            break;
        case Phase::RollingBack:
            status_.missingDevices =
                status_.descriptor.expectedDevices & ~(rolledBack | rejected);
            break;
        case Phase::Active:
            status_.missingDevices = 0;
            break;
        case Phase::Idle:
        case Phase::Rejected:
            break;
    }
    revise();
}

void GraphUpdateCoordinator::handle(const ProtocolMessage &message,
                                    uint32_t nowMilliseconds) {
    if (!configured_ ||
        (message.type != MessageType::GraphStatusIdentity &&
         message.type != MessageType::GraphStatusProgress) ||
        !isPhysicalDevice(message.device) || status_.descriptor.expectedDevices == 0) {
        return;
    }
    NodeObservation &node = nodes_[message.device];
    const uint16_t transferId = message.graph.transferId;
    if (node.transferId != transferId) {
        node = NodeObservation{};
        node.transferId = transferId;
    }

    bool changed = false;
    if (message.type == MessageType::GraphStatusIdentity) {
        changed = !node.hasIdentity || node.generation != message.graph.generation ||
                  node.state != message.graph.state || node.error != message.graph.error;
        node.generation = message.graph.generation;
        node.state = message.graph.state;
        node.error = message.graph.error;
        node.hasIdentity = true;
    } else {
        changed = !node.hasProgress || node.checksum != message.graph.checksum ||
                  node.nextSequence != message.graph.sequence;
        node.checksum = message.graph.checksum;
        node.nextSequence = message.graph.sequence;
        node.hasProgress = true;
    }
    if (changed) {
        lastActivityAt_ = nowMilliseconds;
        recompute(nowMilliseconds);
    }
}

void GraphUpdateCoordinator::service(uint32_t nowMilliseconds) {
    if (!configured_) {
        return;
    }
    const bool timedSession = phase_ == Phase::Announcing || phase_ == Phase::Receiving ||
                              phase_ == Phase::Finishing || phase_ == Phase::Staged ||
                              phase_ == Phase::Activating || phase_ == Phase::RollingBack;
    if (timedSession &&
        timeReached(nowMilliseconds, lastActivityAt_ + kGraphUpdateSessionTimeoutMs)) {
        increment(counters_.timeouts);
        reject(GraphUpdateError::Timeout, nowMilliseconds, true);
        return;
    }
    if (!timeReached(nowMilliseconds, nextSendAt_)) {
        return;
    }

    bool sent = false;
    switch (phase_) {
        case Phase::Announcing:
            if (status_.readyDevices != status_.descriptor.expectedDevices) {
                sent = sendAnnouncementPart(announcementPart_);
                announcementPart_ =
                    static_cast<uint8_t>((announcementPart_ + 1) % kGraphAnnouncementPartCount);
            }
            break;
        case Phase::Receiving:
            if (status_.chunkPending) {
                sent = sendPendingChunk();
            }
            break;
        case Phase::Finishing:
            sent = sendFinish();
            break;
        case Phase::Activating:
            sent = sendActivation();
            break;
        case Phase::RollingBack:
            sent = sendRollback();
            break;
        case Phase::Idle:
        case Phase::Staged:
        case Phase::Active:
        case Phase::Rejected:
            break;
    }
    if (sent || phase_ == Phase::Announcing || status_.chunkPending ||
        phase_ == Phase::Finishing || phase_ == Phase::Activating ||
        phase_ == Phase::RollingBack) {
        nextSendAt_ = nowMilliseconds + kGraphUpdateRetryIntervalMs;
    }
}

}  // namespace piho
