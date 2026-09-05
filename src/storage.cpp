#include "storage.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char *kGraphSlots[piho::kGraphStoreSlotCount] = {
    "/graph.a", "/graph.b", "/graph.c"};
constexpr const char *kGraphMetadata[piho::kGraphStoreMetadataCopyCount] = {
    "/graph.meta.a", "/graph.meta.b"};
constexpr const char *kGraphMetadataTemporary[piho::kGraphStoreMetadataCopyCount] = {
    "/graph.meta.a.tmp", "/graph.meta.b.tmp"};

bool filesystemMounted = false;

bool mountFilesystem() {
    if (!filesystemMounted) {
        filesystemMounted = LittleFS.begin();
    }
    return filesystemMounted;
}

class LittleFsGraphStoreBackend final : public piho::GraphStoreBackend {
   public:
    bool begin() override { return mountFilesystem(); }

    bool readMetadata(uint8_t copy, uint8_t *output, std::size_t capacity,
                      std::size_t &size) override {
        size = 0;
        if (copy >= piho::kGraphStoreMetadataCopyCount) {
            return false;
        }
        const char *path = kGraphMetadata[copy];
        if (!LittleFS.exists(path)) {
            return true;
        }
        File file = LittleFS.open(path, "r");
        if (!file) {
            return false;
        }
        size = file.size();
        if (size > capacity) {
            file.close();
            return true;
        }
        const bool read = file.read(output, size) == static_cast<int>(size);
        file.close();
        return read;
    }

    bool writeMetadata(uint8_t copy, const uint8_t *data, std::size_t size) override {
        if (copy >= piho::kGraphStoreMetadataCopyCount || data == nullptr ||
            size != piho::kGraphStoreMetadataSize) {
            return false;
        }
        const char *temporaryPath = kGraphMetadataTemporary[copy];
        LittleFS.remove(temporaryPath);
        File file = LittleFS.open(temporaryPath, "w");
        if (!file) {
            return false;
        }
        const bool written = file.write(data, size) == size;
        file.flush();
        file.close();
        if (!written) {
            LittleFS.remove(temporaryPath);
            return false;
        }

        uint8_t verified[piho::kGraphStoreMetadataSize]{};
        File verification = LittleFS.open(temporaryPath, "r");
        if (!verification || verification.size() != size ||
            verification.read(verified, size) != static_cast<int>(size)) {
            verification.close();
            LittleFS.remove(temporaryPath);
            return false;
        }
        verification.close();
        if (std::memcmp(data, verified, size) != 0 ||
            !LittleFS.rename(temporaryPath, kGraphMetadata[copy])) {
            LittleFS.remove(temporaryPath);
            return false;
        }
        return true;
    }

    bool beginSlotWrite(uint8_t slot) override {
        if (slot >= piho::kGraphStoreSlotCount || writeFile_) {
            return false;
        }
        writeFile_ = LittleFS.open(kGraphSlots[slot], "w");
        if (!writeFile_) {
            return false;
        }
        writeSlot_ = slot;
        return true;
    }

    bool appendSlot(const uint8_t *data, std::size_t size) override {
        return writeFile_ && data != nullptr && writeFile_.write(data, size) == size;
    }

    bool finishSlotWrite() override {
        if (!writeFile_) {
            return false;
        }
        writeFile_.flush();
        writeFile_.close();
        writeSlot_ = piho::kGraphStoreNoSlot;
        return true;
    }

    void abortSlotWrite() override {
        if (writeFile_) {
            writeFile_.close();
        }
        writeSlot_ = piho::kGraphStoreNoSlot;
    }

    bool eraseSlot(uint8_t slot) override {
        if (slot >= piho::kGraphStoreSlotCount) {
            return false;
        }
        if (writeSlot_ == slot) {
            abortSlotWrite();
        }
        return !LittleFS.exists(kGraphSlots[slot]) || LittleFS.remove(kGraphSlots[slot]);
    }

    bool slotSize(uint8_t slot, std::size_t &size) override {
        size = 0;
        if (slot >= piho::kGraphStoreSlotCount || !LittleFS.exists(kGraphSlots[slot])) {
            return false;
        }
        File file = LittleFS.open(kGraphSlots[slot], "r");
        if (!file) {
            return false;
        }
        size = file.size();
        file.close();
        return true;
    }

    bool readSlot(uint8_t slot, std::size_t offset, uint8_t *output,
                  std::size_t size) const override {
        if (slot >= piho::kGraphStoreSlotCount || output == nullptr) {
            return false;
        }
        File file = LittleFS.open(kGraphSlots[slot], "r");
        if (!file || offset > file.size() || size > file.size() - offset ||
            !file.seek(offset, SeekSet)) {
            file.close();
            return false;
        }
        const bool read = file.read(output, size) == static_cast<int>(size);
        file.close();
        return read;
    }

   private:
    File writeFile_{};
    uint8_t writeSlot_ = piho::kGraphStoreNoSlot;
};

LittleFsGraphStoreBackend graphStoreBackend;

}  // namespace

piho::GraphStore graphStore(graphStoreBackend);
