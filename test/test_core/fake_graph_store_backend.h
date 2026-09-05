#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "piho/graph_store.h"

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
            if (slots[slot].size() < piho::kGraphHeaderSize) {
                continue;
            }
            uint32_t storedGeneration = 0;
            for (uint8_t byte = 0; byte < 4; ++byte) {
                storedGeneration |= static_cast<uint32_t>(slots[slot][8 + byte]) << (byte * 8);
            }
            if (storedGeneration == generation) {
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
