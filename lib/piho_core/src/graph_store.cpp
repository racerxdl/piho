#include "piho/graph_store.h"

#include <cstring>

namespace piho {
namespace {

constexpr uint8_t kMetadataMagic[] = {'P', 'H', 'G', 'S'};
constexpr uint8_t kMetadataVersion = 1;
constexpr std::size_t kMetadataChecksumOffset = 28;

void encodeUint32(uint32_t value, uint8_t *output) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        output[byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

uint32_t decodeUint32(const uint8_t *input) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(input[byte]) << (byte * 8);
    }
    return value;
}

GraphIdentity identityFrom(const GraphManifest &manifest) {
    return GraphIdentity{manifest.format, manifest.executorApi, manifest.generation,
                         manifest.checksum};
}

uint32_t deviceBitmapFrom(const GraphManifest &manifest) {
    uint32_t bitmap = 0;
    for (uint16_t index = 0; index < manifest.deviceCount; ++index) {
        bitmap |= static_cast<uint32_t>(1u) << manifest.devices[index].id;
    }
    return bitmap;
}

bool descriptorsEqual(const GraphReceiveDescriptor &left,
                      const GraphReceiveDescriptor &right) {
    return left.imageSize == right.imageSize && left.generation == right.generation &&
           left.checksum == right.checksum;
}

}  // namespace

bool GraphStore::readSlotSource(const void *context, std::size_t offset, uint8_t *output,
                                std::size_t size) {
    const auto &source = *static_cast<const SlotSourceContext *>(context);
    return source.backend != nullptr &&
           source.backend->readSlot(source.slot, offset, output, size);
}

bool GraphStore::generationIsNewer(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

bool GraphStore::decodeMetadata(const uint8_t *data, std::size_t size, Metadata &metadata) {
    if (data == nullptr || size != kGraphStoreMetadataSize ||
        std::memcmp(data, kMetadataMagic, sizeof(kMetadataMagic)) != 0 ||
        data[4] != kMetadataVersion || data[7] != 0) {
        return false;
    }
    Crc32 checksum;
    checksum.add(data, kMetadataChecksumOffset);
    if (checksum.value() != decodeUint32(&data[kMetadataChecksumOffset])) {
        return false;
    }

    const auto state = static_cast<GraphStoreState>(data[5]);
    const auto lastError = static_cast<GraphStoreError>(data[6]);
    if (state > GraphStoreState::Rollback || lastError > GraphStoreError::NoActiveGraph) {
        return false;
    }
    const uint8_t slots[] = {data[8], data[9], data[10], data[11]};
    bool slotUsed[kGraphStoreSlotCount]{};
    for (uint8_t slot : slots) {
        if (slot == kGraphStoreNoSlot) {
            continue;
        }
        if (slot >= kGraphStoreSlotCount || slotUsed[slot]) {
            return false;
        }
        slotUsed[slot] = true;
    }

    Metadata candidate{};
    candidate.state = state;
    candidate.lastError = lastError;
    candidate.activeSlot = data[8];
    candidate.stagedSlot = data[9];
    candidate.rollbackSlot = data[10];
    candidate.receivingSlot = data[11];
    candidate.sequence = decodeUint32(&data[12]);
    candidate.receive.imageSize = decodeUint32(&data[16]);
    candidate.receive.generation = decodeUint32(&data[20]);
    candidate.receive.checksum = decodeUint32(&data[24]);

    const bool receiving = state == GraphStoreState::Receiving;
    if (receiving != (candidate.receivingSlot != kGraphStoreNoSlot) ||
        (receiving && (candidate.receive.imageSize < kGraphHeaderSize ||
                       candidate.receive.imageSize > kGraphImageCapacity ||
                       candidate.receive.generation == 0)) ||
        (!receiving && (candidate.receive.imageSize != 0 || candidate.receive.generation != 0 ||
                        candidate.receive.checksum != 0)) ||
        (state == GraphStoreState::Staged &&
         candidate.stagedSlot == kGraphStoreNoSlot) ||
        ((state == GraphStoreState::Active || state == GraphStoreState::Rollback) &&
         candidate.activeSlot == kGraphStoreNoSlot) ||
        (state == GraphStoreState::Empty &&
         (candidate.activeSlot != kGraphStoreNoSlot ||
          candidate.stagedSlot != kGraphStoreNoSlot ||
          candidate.rollbackSlot != kGraphStoreNoSlot))) {
        return false;
    }

    metadata = candidate;
    return true;
}

void GraphStore::encodeMetadata(const Metadata &metadata, uint8_t *output) {
    std::memset(output, 0, kGraphStoreMetadataSize);
    std::memcpy(output, kMetadataMagic, sizeof(kMetadataMagic));
    output[4] = kMetadataVersion;
    output[5] = static_cast<uint8_t>(metadata.state);
    output[6] = static_cast<uint8_t>(metadata.lastError);
    output[8] = metadata.activeSlot;
    output[9] = metadata.stagedSlot;
    output[10] = metadata.rollbackSlot;
    output[11] = metadata.receivingSlot;
    encodeUint32(metadata.sequence, &output[12]);
    encodeUint32(metadata.receive.imageSize, &output[16]);
    encodeUint32(metadata.receive.generation, &output[20]);
    encodeUint32(metadata.receive.checksum, &output[24]);
    Crc32 checksum;
    checksum.add(output, kMetadataChecksumOffset);
    encodeUint32(checksum.value(), &output[kMetadataChecksumOffset]);
}

GraphStore::Metadata GraphStore::currentMetadata() const {
    Metadata metadata{};
    metadata.sequence = metadataSequence_;
    metadata.state = status_.state;
    metadata.lastError = status_.lastError;
    metadata.activeSlot = activeSlot_;
    metadata.stagedSlot = stagedSlot_;
    metadata.rollbackSlot = rollbackSlot_;
    metadata.receivingSlot = receivingSlot_;
    metadata.receive = receive_;
    return metadata;
}

void GraphStore::applyMetadata(const Metadata &metadata, uint8_t copy) {
    metadataSequence_ = metadata.sequence;
    metadataCopy_ = copy;
    activeSlot_ = metadata.activeSlot;
    stagedSlot_ = metadata.stagedSlot;
    rollbackSlot_ = metadata.rollbackSlot;
    receivingSlot_ = metadata.receivingSlot;
    receive_ = metadata.receive;
    status_.state = metadata.state;
    status_.lastError = metadata.lastError;
    refreshStatus();
}

GraphStoreError GraphStore::commit(Metadata metadata) {
    ++metadata.sequence;
    if (metadata.sequence == 0) {
        metadata.sequence = 1;
    }
    const uint8_t copy =
        metadataCopy_ == kGraphStoreNoSlot ? 0 : static_cast<uint8_t>(1u - metadataCopy_);
    uint8_t encoded[kGraphStoreMetadataSize]{};
    encodeMetadata(metadata, encoded);
    if (!backend_.writeMetadata(copy, encoded, sizeof(encoded))) {
        status_.lastError = GraphStoreError::MetadataWrite;
        return GraphStoreError::MetadataWrite;
    }

    uint8_t verifiedData[kGraphStoreMetadataSize]{};
    std::size_t verifiedSize = 0;
    Metadata verified{};
    if (!backend_.readMetadata(copy, verifiedData, sizeof(verifiedData), verifiedSize) ||
        !decodeMetadata(verifiedData, verifiedSize, verified) ||
        verified.sequence != metadata.sequence || verified.state != metadata.state ||
        verified.lastError != metadata.lastError ||
        verified.activeSlot != metadata.activeSlot ||
        verified.stagedSlot != metadata.stagedSlot ||
        verified.rollbackSlot != metadata.rollbackSlot ||
        verified.receivingSlot != metadata.receivingSlot ||
        !descriptorsEqual(verified.receive, metadata.receive)) {
        status_.lastError = GraphStoreError::MetadataWrite;
        return GraphStoreError::MetadataWrite;
    }
    applyMetadata(metadata, copy);
    return GraphStoreError::None;
}

GraphStoreError GraphStore::validateSlot(uint8_t slot, GraphManifest &manifest) const {
    if (slot >= kGraphStoreSlotCount) {
        return GraphStoreError::InvalidArgument;
    }
    std::size_t size = 0;
    if (!backend_.slotSize(slot, size)) {
        return GraphStoreError::SlotRead;
    }
    if (size < kGraphHeaderSize || size > kGraphImageCapacity) {
        return GraphStoreError::InvalidLength;
    }
    const SlotSourceContext context{&backend_, slot};
    const GraphImageSource source{&context, size, readSlotSource};
    const GraphImageError error = GraphImageCodec::validate(source, manifest);
    if (error == GraphImageError::None) {
        return GraphStoreError::None;
    }
    if (error == GraphImageError::ReadFailure) {
        return GraphStoreError::SlotRead;
    }
    return error == GraphImageError::InvalidChecksum ? GraphStoreError::InvalidChecksum
                                                      : GraphStoreError::InvalidImage;
}

void GraphStore::clearManifest(GraphManifest &manifest) {
    manifest = GraphManifest{};
}

void GraphStore::refreshStatus() {
    status_.receivedBytes = receivedBytes_;
    status_.expectedBytes =
        status_.state == GraphStoreState::Receiving ? receive_.imageSize : 0;
    status_.active = activeSlot_ == kGraphStoreNoSlot ? GraphIdentity{}
                                                      : identityFrom(activeManifest_);
    status_.staged = stagedSlot_ == kGraphStoreNoSlot ? GraphIdentity{}
                                                      : identityFrom(stagedManifest_);
    status_.rollback = rollbackSlot_ == kGraphStoreNoSlot ? GraphIdentity{}
                                                          : identityFrom(rollbackManifest_);
    status_.activeDevices =
        activeSlot_ == kGraphStoreNoSlot ? 0 : deviceBitmapFrom(activeManifest_);
    status_.stagedDevices =
        stagedSlot_ == kGraphStoreNoSlot ? 0 : deviceBitmapFrom(stagedManifest_);
    status_.rollbackDevices =
        rollbackSlot_ == kGraphStoreNoSlot ? 0 : deviceBitmapFrom(rollbackManifest_);
}

GraphStoreError GraphStore::begin() {
    status_ = GraphStoreStatus{};
    clearManifest(activeManifest_);
    clearManifest(stagedManifest_);
    clearManifest(rollbackManifest_);
    metadataSequence_ = 0;
    metadataCopy_ = kGraphStoreNoSlot;
    activeSlot_ = kGraphStoreNoSlot;
    stagedSlot_ = kGraphStoreNoSlot;
    rollbackSlot_ = kGraphStoreNoSlot;
    receivingSlot_ = kGraphStoreNoSlot;
    receive_ = GraphReceiveDescriptor{};
    receivedBytes_ = 0;
    receivingOpen_ = false;
    mounted_ = false;

    if (!backend_.begin()) {
        status_.state = GraphStoreState::Invalid;
        status_.lastError = GraphStoreError::Mount;
        return GraphStoreError::Mount;
    }

    uint8_t encoded[kGraphStoreMetadataCopyCount][kGraphStoreMetadataSize]{};
    std::size_t sizes[kGraphStoreMetadataCopyCount]{};
    Metadata metadata[kGraphStoreMetadataCopyCount]{};
    bool valid[kGraphStoreMetadataCopyCount]{};
    bool anyStoredMetadata = false;
    for (uint8_t copy = 0; copy < kGraphStoreMetadataCopyCount; ++copy) {
        if (!backend_.readMetadata(copy, encoded[copy], sizeof(encoded[copy]), sizes[copy])) {
            status_.state = GraphStoreState::Invalid;
            status_.lastError = GraphStoreError::MetadataRead;
            return GraphStoreError::MetadataRead;
        }
        anyStoredMetadata = anyStoredMetadata || sizes[copy] != 0;
        valid[copy] = decodeMetadata(encoded[copy], sizes[copy], metadata[copy]);
    }

    if (!valid[0] && !valid[1]) {
        Metadata initial{};
        initial.state = anyStoredMetadata ? GraphStoreState::Invalid : GraphStoreState::Empty;
        initial.lastError =
            anyStoredMetadata ? GraphStoreError::MetadataCorrupt : GraphStoreError::None;
        initial.activeSlot = kGraphStoreNoSlot;
        initial.stagedSlot = kGraphStoreNoSlot;
        initial.rollbackSlot = kGraphStoreNoSlot;
        initial.receivingSlot = kGraphStoreNoSlot;
        const GraphStoreError error = commit(initial);
        mounted_ = error == GraphStoreError::None;
        return error;
    }

    const uint8_t selected =
        valid[0] && (!valid[1] || !generationIsNewer(metadata[1].sequence, metadata[0].sequence))
            ? 0
            : 1;
    applyMetadata(metadata[selected], selected);

    Metadata recovered = currentMetadata();
    bool changed = false;
    if (recovered.state == GraphStoreState::Receiving) {
        backend_.abortSlotWrite();
        backend_.eraseSlot(recovered.receivingSlot);
        recovered.receivingSlot = kGraphStoreNoSlot;
        recovered.receive = GraphReceiveDescriptor{};
        recovered.state = GraphStoreState::Invalid;
        recovered.lastError = GraphStoreError::Interrupted;
        changed = true;
    }

    GraphStoreError activeError = GraphStoreError::None;
    GraphStoreError stagedError = GraphStoreError::None;
    GraphStoreError rollbackError = GraphStoreError::None;
    if (recovered.activeSlot != kGraphStoreNoSlot) {
        activeError = validateSlot(recovered.activeSlot, activeManifest_);
    }
    if (recovered.stagedSlot != kGraphStoreNoSlot) {
        stagedError = validateSlot(recovered.stagedSlot, stagedManifest_);
    }
    if (recovered.rollbackSlot != kGraphStoreNoSlot) {
        rollbackError = validateSlot(recovered.rollbackSlot, rollbackManifest_);
    }

    if (activeError != GraphStoreError::None) {
        clearManifest(activeManifest_);
        if (recovered.rollbackSlot != kGraphStoreNoSlot &&
            rollbackError == GraphStoreError::None) {
            recovered.activeSlot = recovered.rollbackSlot;
            recovered.rollbackSlot = kGraphStoreNoSlot;
            activeManifest_ = rollbackManifest_;
            clearManifest(rollbackManifest_);
            recovered.state = GraphStoreState::Rollback;
        } else {
            recovered.activeSlot = kGraphStoreNoSlot;
            recovered.state = GraphStoreState::Invalid;
        }
        recovered.lastError = activeError;
        changed = true;
    }
    if (stagedError != GraphStoreError::None) {
        recovered.stagedSlot = kGraphStoreNoSlot;
        clearManifest(stagedManifest_);
        recovered.state = GraphStoreState::Invalid;
        recovered.lastError = stagedError;
        changed = true;
    }
    if (recovered.rollbackSlot != kGraphStoreNoSlot &&
        rollbackError != GraphStoreError::None) {
        recovered.rollbackSlot = kGraphStoreNoSlot;
        clearManifest(rollbackManifest_);
        recovered.lastError = rollbackError;
        changed = true;
    }

    if (changed) {
        const GraphStoreError error = commit(recovered);
        if (error != GraphStoreError::None) {
            return error;
        }
    } else {
        refreshStatus();
    }
    mounted_ = true;
    return GraphStoreError::None;
}

uint8_t GraphStore::availableSlot() const {
    if (stagedSlot_ != kGraphStoreNoSlot) {
        return stagedSlot_;
    }
    for (uint8_t slot = 0; slot < kGraphStoreSlotCount; ++slot) {
        if (slot != activeSlot_ && slot != rollbackSlot_) {
            return slot;
        }
    }
    return kGraphStoreNoSlot;
}

GraphStoreError GraphStore::beginReceive(const GraphReceiveDescriptor &descriptor) {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state == GraphStoreState::Receiving || receivingOpen_) {
        return GraphStoreError::Busy;
    }
    if (descriptor.imageSize < kGraphHeaderSize || descriptor.imageSize > kGraphImageCapacity ||
        descriptor.generation == 0 ||
        (activeSlot_ != kGraphStoreNoSlot &&
         descriptor.generation == activeManifest_.generation) ||
        (rollbackSlot_ != kGraphStoreNoSlot &&
         descriptor.generation == rollbackManifest_.generation)) {
        return GraphStoreError::InvalidArgument;
    }
    const uint8_t slot = availableSlot();
    if (slot == kGraphStoreNoSlot) {
        return GraphStoreError::NoSlot;
    }

    Metadata receiving = currentMetadata();
    receiving.state = GraphStoreState::Receiving;
    receiving.lastError = GraphStoreError::None;
    receiving.stagedSlot = kGraphStoreNoSlot;
    receiving.receivingSlot = slot;
    receiving.receive = descriptor;
    const GraphStoreError metadataError = commit(receiving);
    if (metadataError != GraphStoreError::None) {
        return metadataError;
    }
    clearManifest(stagedManifest_);
    if (!backend_.beginSlotWrite(slot)) {
        return failReceive(GraphStoreError::SlotOpen);
    }
    receivingOpen_ = true;
    receivedBytes_ = 0;
    receiveChecksum_ = Crc32{};
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::writeChunk(const uint8_t *data, std::size_t size) {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state != GraphStoreState::Receiving || !receivingOpen_) {
        return GraphStoreError::Busy;
    }
    if (data == nullptr || size == 0 || size > kGraphStoreChunkCapacity ||
        size > receive_.imageSize - receivedBytes_) {
        return failReceive(GraphStoreError::InvalidArgument);
    }
    if (!backend_.appendSlot(data, size)) {
        return failReceive(GraphStoreError::SlotWrite);
    }
    for (std::size_t index = 0; index < size; ++index) {
        const std::size_t imageOffset = receivedBytes_ + index;
        receiveChecksum_.add(imageOffset >= kGraphChecksumOffset &&
                                     imageOffset < kGraphChecksumOffset + sizeof(uint32_t)
                                 ? 0
                                 : data[index]);
    }
    receivedBytes_ += static_cast<uint32_t>(size);
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::failReceive(GraphStoreError error) {
    if (receivingOpen_) {
        backend_.abortSlotWrite();
        receivingOpen_ = false;
    }
    if (receivingSlot_ != kGraphStoreNoSlot) {
        backend_.eraseSlot(receivingSlot_);
    }
    Metadata invalid = currentMetadata();
    invalid.state = GraphStoreState::Invalid;
    invalid.lastError = error;
    invalid.receivingSlot = kGraphStoreNoSlot;
    invalid.receive = GraphReceiveDescriptor{};
    receivedBytes_ = 0;
    const GraphStoreError metadataError = commit(invalid);
    return metadataError == GraphStoreError::None ? error : metadataError;
}

GraphStoreError GraphStore::finishReceive() {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state != GraphStoreState::Receiving || !receivingOpen_) {
        return GraphStoreError::Busy;
    }
    if (receivedBytes_ != receive_.imageSize) {
        return failReceive(GraphStoreError::InvalidLength);
    }
    if (!backend_.finishSlotWrite()) {
        return failReceive(GraphStoreError::SlotClose);
    }
    receivingOpen_ = false;
    if (receiveChecksum_.value() != receive_.checksum) {
        return failReceive(GraphStoreError::InvalidChecksum);
    }

    GraphManifest manifest{};
    const GraphStoreError validationError = validateSlot(receivingSlot_, manifest);
    if (validationError != GraphStoreError::None || manifest.imageSize != receive_.imageSize ||
        manifest.generation != receive_.generation || manifest.checksum != receive_.checksum) {
        return failReceive(validationError == GraphStoreError::None
                               ? GraphStoreError::InvalidImage
                               : validationError);
    }

    const uint8_t stagedSlot = receivingSlot_;
    Metadata staged = currentMetadata();
    staged.state = GraphStoreState::Staged;
    staged.lastError = GraphStoreError::None;
    staged.stagedSlot = stagedSlot;
    staged.receivingSlot = kGraphStoreNoSlot;
    staged.receive = GraphReceiveDescriptor{};
    const GraphStoreError metadataError = commit(staged);
    if (metadataError != GraphStoreError::None) {
        return metadataError;
    }
    stagedManifest_ = manifest;
    receivedBytes_ = 0;
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::cancelReceive() {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state != GraphStoreState::Receiving) {
        return GraphStoreError::Busy;
    }
    return failReceive(GraphStoreError::Interrupted);
}

GraphStoreError GraphStore::discardStaged() {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state == GraphStoreState::Receiving) {
        return GraphStoreError::Busy;
    }
    if (stagedSlot_ == kGraphStoreNoSlot) {
        return GraphStoreError::NoStagedGraph;
    }
    const uint8_t discardedSlot = stagedSlot_;
    Metadata discarded = currentMetadata();
    discarded.stagedSlot = kGraphStoreNoSlot;
    discarded.state = activeSlot_ != kGraphStoreNoSlot
                          ? GraphStoreState::Active
                          : (rollbackSlot_ != kGraphStoreNoSlot ? GraphStoreState::Rollback
                                                                : GraphStoreState::Empty);
    discarded.lastError = GraphStoreError::None;
    const GraphStoreError error = commit(discarded);
    if (error != GraphStoreError::None) {
        return error;
    }
    clearManifest(stagedManifest_);
    backend_.eraseSlot(discardedSlot);
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::activate() {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state == GraphStoreState::Receiving) {
        return GraphStoreError::Busy;
    }
    if (stagedSlot_ == kGraphStoreNoSlot) {
        return GraphStoreError::NoStagedGraph;
    }
    GraphManifest stagedManifest{};
    const GraphStoreError validationError = validateSlot(stagedSlot_, stagedManifest);
    if (validationError != GraphStoreError::None) {
        const uint8_t invalidSlot = stagedSlot_;
        Metadata invalid = currentMetadata();
        invalid.state = GraphStoreState::Invalid;
        invalid.lastError = validationError;
        invalid.stagedSlot = kGraphStoreNoSlot;
        const GraphStoreError metadataError = commit(invalid);
        clearManifest(stagedManifest_);
        backend_.eraseSlot(invalidSlot);
        refreshStatus();
        return metadataError == GraphStoreError::None ? validationError : metadataError;
    }

    const GraphManifest previousActiveManifest = activeManifest_;
    Metadata activated = currentMetadata();
    activated.state = GraphStoreState::Active;
    activated.lastError = GraphStoreError::None;
    activated.rollbackSlot = activeSlot_;
    activated.activeSlot = stagedSlot_;
    activated.stagedSlot = kGraphStoreNoSlot;
    const GraphStoreError metadataError = commit(activated);
    if (metadataError != GraphStoreError::None) {
        return metadataError;
    }
    activeManifest_ = stagedManifest;
    rollbackManifest_ = previousActiveManifest;
    clearManifest(stagedManifest_);
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::rollback() {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (status_.state == GraphStoreState::Receiving) {
        return GraphStoreError::Busy;
    }
    if (rollbackSlot_ == kGraphStoreNoSlot) {
        return GraphStoreError::NoRollbackGraph;
    }
    GraphManifest rollbackManifest{};
    const GraphStoreError validationError = validateSlot(rollbackSlot_, rollbackManifest);
    if (validationError != GraphStoreError::None) {
        const uint8_t invalidSlot = rollbackSlot_;
        Metadata invalid = currentMetadata();
        invalid.state = GraphStoreState::Invalid;
        invalid.lastError = validationError;
        invalid.rollbackSlot = kGraphStoreNoSlot;
        const GraphStoreError metadataError = commit(invalid);
        clearManifest(rollbackManifest_);
        backend_.eraseSlot(invalidSlot);
        refreshStatus();
        return metadataError == GraphStoreError::None ? validationError : metadataError;
    }

    const GraphManifest previousActiveManifest = activeManifest_;
    Metadata rolledBack = currentMetadata();
    const uint8_t previousActiveSlot = rolledBack.activeSlot;
    rolledBack.activeSlot = rolledBack.rollbackSlot;
    rolledBack.rollbackSlot = previousActiveSlot;
    rolledBack.state = GraphStoreState::Rollback;
    rolledBack.lastError = GraphStoreError::None;
    const GraphStoreError metadataError = commit(rolledBack);
    if (metadataError != GraphStoreError::None) {
        return metadataError;
    }
    activeManifest_ = rollbackManifest;
    rollbackManifest_ = previousActiveManifest;
    refreshStatus();
    return GraphStoreError::None;
}

GraphStoreError GraphStore::loadActiveInput(uint8_t device, LocalInputGraph &staging) {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (activeSlot_ == kGraphStoreNoSlot) {
        return GraphStoreError::NoActiveGraph;
    }
    GraphManifest manifest{};
    const GraphStoreError validationError = validateSlot(activeSlot_, manifest);
    if (validationError != GraphStoreError::None) {
        status_.lastError = validationError;
        return validationError;
    }
    std::size_t size = 0;
    if (!backend_.slotSize(activeSlot_, size)) {
        status_.lastError = GraphStoreError::SlotRead;
        return GraphStoreError::SlotRead;
    }
    const SlotSourceContext context{&backend_, activeSlot_};
    const GraphImageSource source{&context, size, readSlotSource};
    if (GraphImageCodec::loadInputSection(source, manifest, device, staging) !=
        GraphImageError::None) {
        status_.lastError = GraphStoreError::InvalidImage;
        return GraphStoreError::InvalidImage;
    }
    return GraphStoreError::None;
}

GraphStoreError GraphStore::loadActiveOutput(uint8_t device, LocalOutputGraph &staging) {
    if (!mounted_) {
        return GraphStoreError::Mount;
    }
    if (activeSlot_ == kGraphStoreNoSlot) {
        return GraphStoreError::NoActiveGraph;
    }
    GraphManifest manifest{};
    const GraphStoreError validationError = validateSlot(activeSlot_, manifest);
    if (validationError != GraphStoreError::None) {
        status_.lastError = validationError;
        return validationError;
    }
    std::size_t size = 0;
    if (!backend_.slotSize(activeSlot_, size)) {
        status_.lastError = GraphStoreError::SlotRead;
        return GraphStoreError::SlotRead;
    }
    const SlotSourceContext context{&backend_, activeSlot_};
    const GraphImageSource source{&context, size, readSlotSource};
    if (GraphImageCodec::loadOutputSection(source, manifest, device, staging) !=
        GraphImageError::None) {
        status_.lastError = GraphStoreError::InvalidImage;
        return GraphStoreError::InvalidImage;
    }
    return GraphStoreError::None;
}

}  // namespace piho
