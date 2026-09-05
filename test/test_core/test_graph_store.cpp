#include <unity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "piho/graph_image.h"
#include "piho/graph_store.h"

namespace {

constexpr const char *kGoldenPath = "tools/piho-flow/test/fixtures/synthetic.phg";

uint32_t readUint32(const uint8_t *data) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(data[byte]) << (byte * 8);
    }
    return value;
}

void writeUint32(std::vector<uint8_t> &data, std::size_t offset, uint32_t value) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        data[offset + byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

std::vector<uint8_t> graphImage(uint32_t generation) {
    std::ifstream input(kGoldenPath, std::ios::binary);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "cannot open shared synthetic.phg fixture");
    std::vector<uint8_t> image{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    writeUint32(image, 8, generation);
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::checksum(
            piho::GraphImageSource::fromMemory(image.data(), image.size()), checksum)));
    writeUint32(image, piho::kGraphChecksumOffset, checksum);
    return image;
}

piho::GraphReceiveDescriptor descriptor(const std::vector<uint8_t> &image) {
    return piho::GraphReceiveDescriptor{static_cast<uint32_t>(image.size()),
                                        readUint32(&image[8]),
                                        readUint32(&image[piho::kGraphChecksumOffset])};
}

class FakeGraphStoreBackend final : public piho::GraphStoreBackend {
   public:
    bool begin() override {
        writeSlot = piho::kGraphStoreNoSlot;
        return !failMount;
    }

    bool readMetadata(uint8_t copy, uint8_t *output, std::size_t capacity,
                      std::size_t &size) override {
        size = 0;
        if (failMetadataReads || copy >= metadata.size()) {
            return false;
        }
        const std::vector<uint8_t> &stored = metadata[copy];
        if (stored.size() > capacity) {
            return false;
        }
        if (!stored.empty()) {
            std::memcpy(output, stored.data(), stored.size());
        }
        size = stored.size();
        return true;
    }

    bool writeMetadata(uint8_t copy, const uint8_t *data, std::size_t size) override {
        if (failNextMetadataWrite) {
            failNextMetadataWrite = false;
            return false;
        }
        if (copy >= metadata.size() || data == nullptr) {
            return false;
        }
        metadata[copy].assign(data, data + size);
        return true;
    }

    bool beginSlotWrite(uint8_t slot) override {
        if (failNextSlotOpen) {
            failNextSlotOpen = false;
            return false;
        }
        if (slot >= slots.size() || writeSlot != piho::kGraphStoreNoSlot) {
            return false;
        }
        slots[slot].clear();
        writeSlot = slot;
        return true;
    }

    bool appendSlot(const uint8_t *data, std::size_t size) override {
        if (failNextAppend) {
            failNextAppend = false;
            return false;
        }
        if (writeSlot >= slots.size() || data == nullptr) {
            return false;
        }
        slots[writeSlot].insert(slots[writeSlot].end(), data, data + size);
        return true;
    }

    bool finishSlotWrite() override {
        if (failNextFinish) {
            failNextFinish = false;
            return false;
        }
        if (writeSlot >= slots.size()) {
            return false;
        }
        writeSlot = piho::kGraphStoreNoSlot;
        return true;
    }

    void abortSlotWrite() override { writeSlot = piho::kGraphStoreNoSlot; }

    bool eraseSlot(uint8_t slot) override {
        if (slot >= slots.size()) {
            return false;
        }
        slots[slot].clear();
        if (writeSlot == slot) {
            writeSlot = piho::kGraphStoreNoSlot;
        }
        return true;
    }

    bool slotSize(uint8_t slot, std::size_t &size) override {
        if (slot >= slots.size()) {
            return false;
        }
        size = slots[slot].size();
        return true;
    }

    bool readSlot(uint8_t slot, std::size_t offset, uint8_t *output,
                  std::size_t size) const override {
        if (failNextSlotRead) {
            failNextSlotRead = false;
            return false;
        }
        if (slot >= slots.size() || output == nullptr || offset > slots[slot].size() ||
            size > slots[slot].size() - offset) {
            return false;
        }
        if (size > largestSlotRead) {
            largestSlotRead = size;
        }
        std::memcpy(output, slots[slot].data() + offset, size);
        return true;
    }

    uint8_t slotWithGeneration(uint32_t generation) const {
        for (uint8_t slot = 0; slot < slots.size(); ++slot) {
            if (slots[slot].size() >= piho::kGraphHeaderSize &&
                readUint32(&slots[slot][8]) == generation) {
                return slot;
            }
        }
        return piho::kGraphStoreNoSlot;
    }

    std::array<std::vector<uint8_t>, piho::kGraphStoreSlotCount> slots{};
    std::array<std::vector<uint8_t>, piho::kGraphStoreMetadataCopyCount> metadata{};
    uint8_t writeSlot = piho::kGraphStoreNoSlot;
    mutable std::size_t largestSlotRead = 0;
    bool failMount = false;
    bool failMetadataReads = false;
    bool failNextMetadataWrite = false;
    bool failNextSlotOpen = false;
    bool failNextAppend = false;
    bool failNextFinish = false;
    mutable bool failNextSlotRead = false;
};

void stage(piho::GraphStore &store, const std::vector<uint8_t> &image) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(descriptor(image))));
    for (std::size_t offset = 0; offset < image.size();) {
        const std::size_t remaining = image.size() - offset;
        const std::size_t chunk = remaining < 37 ? remaining : 37;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(store.writeChunk(image.data() + offset, chunk)));
        offset += chunk;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.finishReceive()));
}

void makeActive(piho::GraphStore &store, const std::vector<uint8_t> &image) {
    stage(store, image);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.activate()));
}

void test_graph_store_streams_stages_activates_loads_and_rolls_back() {
    FakeGraphStoreBackend backend;
    piho::GraphStore store(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreState::Empty),
                            static_cast<uint8_t>(store.status().state));

    const std::vector<uint8_t> first = graphImage(1);
    const std::vector<uint8_t> second = graphImage(2);
    const std::vector<uint8_t> third = graphImage(3);
    stage(store, first);
    TEST_ASSERT_FALSE(store.hasActiveGraph());
    TEST_ASSERT_TRUE(store.hasStagedGraph());
    TEST_ASSERT_EQUAL_UINT32(1, store.status().staged.generation);

    piho::GraphStore rebooted(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.begin()));
    TEST_ASSERT_FALSE(rebooted.hasActiveGraph());
    TEST_ASSERT_TRUE(rebooted.hasStagedGraph());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreState::Staged),
                            static_cast<uint8_t>(rebooted.status().state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.activate()));
    TEST_ASSERT_EQUAL_UINT32(1, rebooted.status().active.generation);
    TEST_ASSERT_FALSE(rebooted.hasRollbackGraph());

    piho::LocalInputGraph input{};
    piho::LocalOutputGraph output{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.loadActiveInput(1, input)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.loadActiveOutput(7, output)));
    TEST_ASSERT_EQUAL_UINT32(input.identity.generation, output.identity.generation);
    TEST_ASSERT_EQUAL_HEX32(input.identity.checksum, output.identity.checksum);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.beginReceive(descriptor(second))));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreState::Receiving),
                            static_cast<uint8_t>(rebooted.status().state));
    TEST_ASSERT_EQUAL_UINT32(1, rebooted.status().active.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.loadActiveInput(1, input)));
    for (std::size_t offset = 0; offset < second.size();) {
        const std::size_t remaining = second.size() - offset;
        const std::size_t chunk = remaining < 31 ? remaining : 31;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(rebooted.writeChunk(second.data() + offset, chunk)));
        offset += chunk;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.finishReceive()));
    TEST_ASSERT_EQUAL_UINT32(1, rebooted.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, rebooted.status().staged.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.activate()));
    TEST_ASSERT_EQUAL_UINT32(2, rebooted.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, rebooted.status().rollback.generation);

    stage(rebooted, third);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.activate()));
    TEST_ASSERT_EQUAL_UINT32(3, rebooted.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, rebooted.status().rollback.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(rebooted.rollback()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreState::Rollback),
                            static_cast<uint8_t>(rebooted.status().state));
    TEST_ASSERT_EQUAL_UINT32(2, rebooted.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(3, rebooted.status().rollback.generation);

    piho::GraphStore finalBoot(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(finalBoot.begin()));
    TEST_ASSERT_EQUAL_UINT32(2, finalBoot.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(3, finalBoot.status().rollback.generation);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(64, backend.largestSlotRead);
}

void test_graph_store_recovers_interrupted_receives_without_replacing_active() {
    FakeGraphStoreBackend backend;
    piho::GraphStore store(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    const std::vector<uint8_t> first = graphImage(1);
    const std::vector<uint8_t> second = graphImage(2);
    makeActive(store, first);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    piho::GraphStore afterBeginLoss(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterBeginLoss.begin()));
    TEST_ASSERT_EQUAL_UINT32(1, afterBeginLoss.status().active.generation);
    TEST_ASSERT_FALSE(afterBeginLoss.hasStagedGraph());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Interrupted),
                            static_cast<uint8_t>(afterBeginLoss.status().lastError));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterBeginLoss.beginReceive(descriptor(second))));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterBeginLoss.writeChunk(second.data(), 80)));
    piho::GraphStore afterChunkLoss(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterChunkLoss.begin()));
    TEST_ASSERT_EQUAL_UINT32(1, afterChunkLoss.status().active.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Interrupted),
                            static_cast<uint8_t>(afterChunkLoss.status().lastError));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterChunkLoss.beginReceive(descriptor(second))));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterChunkLoss.writeChunk(second.data(), 80)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::InvalidLength),
                            static_cast<uint8_t>(afterChunkLoss.finishReceive()));
    TEST_ASSERT_EQUAL_UINT32(1, afterChunkLoss.status().active.generation);

    std::vector<uint8_t> badChecksum = second;
    badChecksum[100] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterChunkLoss.beginReceive(descriptor(second))));
    for (std::size_t offset = 0; offset < badChecksum.size();) {
        const std::size_t chunk = (badChecksum.size() - offset) < 53
                                      ? badChecksum.size() - offset
                                      : 53;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(afterChunkLoss.writeChunk(badChecksum.data() + offset, chunk)));
        offset += chunk;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::InvalidChecksum),
                            static_cast<uint8_t>(afterChunkLoss.finishReceive()));
    TEST_ASSERT_EQUAL_UINT32(1, afterChunkLoss.status().active.generation);

    std::vector<uint8_t> invalidImage = second;
    invalidImage[0] = 'X';
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::checksum(
            piho::GraphImageSource::fromMemory(invalidImage.data(), invalidImage.size()),
            checksum)));
    writeUint32(invalidImage, piho::kGraphChecksumOffset, checksum);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterChunkLoss.beginReceive(descriptor(invalidImage))));
    for (std::size_t offset = 0; offset < invalidImage.size();) {
        const std::size_t chunk = (invalidImage.size() - offset) < 64
                                      ? invalidImage.size() - offset
                                      : 64;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(afterChunkLoss.writeChunk(invalidImage.data() + offset, chunk)));
        offset += chunk;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::InvalidImage),
                            static_cast<uint8_t>(afterChunkLoss.finishReceive()));
    TEST_ASSERT_EQUAL_UINT32(1, afterChunkLoss.status().active.generation);
}

void test_graph_store_storage_failures_leave_durable_state_recoverable() {
    FakeGraphStoreBackend backend;
    piho::GraphStore store(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    const std::vector<uint8_t> first = graphImage(1);
    const std::vector<uint8_t> second = graphImage(2);
    const std::vector<uint8_t> third = graphImage(3);
    makeActive(store, first);

    backend.failNextMetadataWrite = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    TEST_ASSERT_EQUAL_UINT32(1, store.status().active.generation);

    backend.failNextSlotOpen = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::SlotOpen),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    TEST_ASSERT_EQUAL_UINT32(1, store.status().active.generation);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    backend.failNextAppend = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::SlotWrite),
                            static_cast<uint8_t>(store.writeChunk(second.data(), 32)));
    TEST_ASSERT_EQUAL_UINT32(1, store.status().active.generation);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    for (std::size_t offset = 0; offset < second.size();) {
        const std::size_t chunk = (second.size() - offset) < 64 ? second.size() - offset : 64;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(store.writeChunk(second.data() + offset, chunk)));
        offset += chunk;
    }
    backend.failNextFinish = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::SlotClose),
                            static_cast<uint8_t>(store.finishReceive()));
    TEST_ASSERT_EQUAL_UINT32(1, store.status().active.generation);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(descriptor(second))));
    for (std::size_t offset = 0; offset < second.size();) {
        const std::size_t chunk = (second.size() - offset) < 64 ? second.size() - offset : 64;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(store.writeChunk(second.data() + offset, chunk)));
        offset += chunk;
    }
    backend.failNextMetadataWrite = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(store.finishReceive()));
    piho::GraphStore afterFinishLoss(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterFinishLoss.begin()));
    TEST_ASSERT_EQUAL_UINT32(1, afterFinishLoss.status().active.generation);
    TEST_ASSERT_FALSE(afterFinishLoss.hasStagedGraph());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Interrupted),
                            static_cast<uint8_t>(afterFinishLoss.status().lastError));

    stage(afterFinishLoss, second);
    backend.failNextMetadataWrite = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(afterFinishLoss.activate()));
    TEST_ASSERT_EQUAL_UINT32(1, afterFinishLoss.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, afterFinishLoss.status().staged.generation);

    piho::GraphStore afterActivationLoss(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterActivationLoss.begin()));
    TEST_ASSERT_EQUAL_UINT32(1, afterActivationLoss.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, afterActivationLoss.status().staged.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterActivationLoss.activate()));
    TEST_ASSERT_EQUAL_UINT32(2, afterActivationLoss.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, afterActivationLoss.status().rollback.generation);

    backend.failNextMetadataWrite = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(afterActivationLoss.rollback()));
    TEST_ASSERT_EQUAL_UINT32(2, afterActivationLoss.status().active.generation);
    piho::GraphStore afterRollbackLoss(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterRollbackLoss.begin()));
    TEST_ASSERT_EQUAL_UINT32(2, afterRollbackLoss.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, afterRollbackLoss.status().rollback.generation);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(afterRollbackLoss.rollback()));
    TEST_ASSERT_EQUAL_UINT32(1, afterRollbackLoss.status().active.generation);

    stage(afterRollbackLoss, third);
    const uint8_t stagedSlot = backend.slotWithGeneration(3);
    TEST_ASSERT_LESS_THAN_UINT8(piho::kGraphStoreSlotCount, stagedSlot);
    backend.slots[stagedSlot][100] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::InvalidChecksum),
                            static_cast<uint8_t>(afterRollbackLoss.activate()));
    TEST_ASSERT_EQUAL_UINT32(1, afterRollbackLoss.status().active.generation);
    TEST_ASSERT_FALSE(afterRollbackLoss.hasStagedGraph());
}

void test_graph_store_uses_older_metadata_when_newest_copy_is_corrupt() {
    FakeGraphStoreBackend backend;
    piho::GraphStore store(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    const std::vector<uint8_t> first = graphImage(1);
    const std::vector<uint8_t> second = graphImage(2);
    makeActive(store, first);
    stage(store, second);

    uint8_t newestCopy = 0;
    if (readUint32(&backend.metadata[1][12]) > readUint32(&backend.metadata[0][12])) {
        newestCopy = 1;
    }
    backend.metadata[newestCopy][0] ^= 0x01;
    piho::GraphStore recovered(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(recovered.begin()));
    TEST_ASSERT_EQUAL_UINT32(1, recovered.status().active.generation);
    TEST_ASSERT_FALSE(recovered.hasStagedGraph());
}

void test_graph_store_promotes_valid_rollback_when_active_image_is_corrupt() {
    FakeGraphStoreBackend backend;
    piho::GraphStore store(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    const std::vector<uint8_t> first = graphImage(1);
    const std::vector<uint8_t> second = graphImage(2);
    makeActive(store, first);
    stage(store, second);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.activate()));

    const uint8_t activeSlot = backend.slotWithGeneration(2);
    TEST_ASSERT_LESS_THAN_UINT8(piho::kGraphStoreSlotCount, activeSlot);
    backend.slots[activeSlot][100] ^= 0x01;

    piho::GraphStore recovered(backend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(recovered.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreState::Rollback),
                            static_cast<uint8_t>(recovered.status().state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::InvalidChecksum),
                            static_cast<uint8_t>(recovered.status().lastError));
    TEST_ASSERT_EQUAL_UINT32(1, recovered.status().active.generation);
    TEST_ASSERT_FALSE(recovered.hasRollbackGraph());
}

void test_graph_store_reports_initialization_failures_and_stays_unavailable() {
    const std::vector<uint8_t> image = graphImage(1);

    FakeGraphStoreBackend mountBackend;
    mountBackend.failMount = true;
    piho::GraphStore mountFailure(mountBackend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Mount),
                            static_cast<uint8_t>(mountFailure.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Mount),
                            static_cast<uint8_t>(mountFailure.status().lastError));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Mount),
                            static_cast<uint8_t>(mountFailure.beginReceive(descriptor(image))));

    FakeGraphStoreBackend readBackend;
    readBackend.failMetadataReads = true;
    piho::GraphStore readFailure(readBackend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataRead),
                            static_cast<uint8_t>(readFailure.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataRead),
                            static_cast<uint8_t>(readFailure.status().lastError));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Mount),
                            static_cast<uint8_t>(readFailure.beginReceive(descriptor(image))));

    FakeGraphStoreBackend writeBackend;
    writeBackend.failNextMetadataWrite = true;
    piho::GraphStore writeFailure(writeBackend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(writeFailure.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::MetadataWrite),
                            static_cast<uint8_t>(writeFailure.status().lastError));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::Mount),
                            static_cast<uint8_t>(writeFailure.beginReceive(descriptor(image))));
}

}  // namespace

void runGraphStoreTests() {
    RUN_TEST(test_graph_store_streams_stages_activates_loads_and_rolls_back);
    RUN_TEST(test_graph_store_recovers_interrupted_receives_without_replacing_active);
    RUN_TEST(test_graph_store_storage_failures_leave_durable_state_recoverable);
    RUN_TEST(test_graph_store_uses_older_metadata_when_newest_copy_is_corrupt);
    RUN_TEST(test_graph_store_promotes_valid_rollback_when_active_image_is_corrupt);
    RUN_TEST(test_graph_store_reports_initialization_failures_and_stays_unavailable);
}
