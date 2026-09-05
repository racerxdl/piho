#include "storage.h"

#include <Arduino.h>
#include <LittleFS.h>

#include <cstddef>
#include <cstdint>

#include "piho/trigger_storage_codec.h"

TriggerStorage triggerStorage;
piho::TriggerTable triggerRules;

namespace {

constexpr char kSlotA[] = "/triggers.a";
constexpr char kSlotB[] = "/triggers.b";
constexpr char kLegacyPath[] = "/triggers.bin";

bool tablesEqual(const piho::TriggerTable &lhs, const piho::TriggerTable &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!(lhs.at(index) == rhs.at(index))) {
            return false;
        }
    }
    return true;
}

bool readSlot(const char *path, piho::TriggerTable &rules, uint32_t &generation) {
    File file = LittleFS.open(path, "r");
    if (!file) {
        return false;
    }

    const std::size_t size = file.size();
    if (size > piho::kTriggerStorageCapacity) {
        file.close();
        return false;
    }

    uint8_t image[piho::kTriggerStorageCapacity]{};
    const bool read = file.read(image, size) == static_cast<int>(size);
    file.close();
    return read && piho::TriggerStorageCodec::decode(image, size, rules, generation) ==
                       piho::TriggerStorageError::None;
}

bool writeSlot(const char *path, const piho::TriggerTable &rules, uint32_t generation) {
    uint8_t image[piho::kTriggerStorageCapacity]{};
    std::size_t size = 0;
    if (piho::TriggerStorageCodec::encode(rules, generation, image, sizeof(image), size) !=
        piho::TriggerStorageError::None) {
        return false;
    }

    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }
    const bool written = file.write(image, size) == size;
    file.flush();
    file.close();
    return written;
}

bool generationIsNewer(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
}

}  // namespace

bool TriggerStorage::begin(piho::TriggerTable &rules) {
    mounted_ = LittleFS.begin();
    if (!mounted_) {
        return false;
    }

    piho::TriggerTable slotA;
    piho::TriggerTable slotB;
    uint32_t generationA = 0;
    uint32_t generationB = 0;
    const bool validA = readSlot(kSlotA, slotA, generationA);
    const bool validB = readSlot(kSlotB, slotB, generationB);

    if (validA && (!validB || !generationIsNewer(generationB, generationA))) {
        rules = slotA;
        generation_ = generationA;
        activeSlot_ = 0;
    } else if (validB) {
        rules = slotB;
        generation_ = generationB;
        activeSlot_ = 1;
    } else {
        rules.clear();
        generation_ = 0;
        activeSlot_ = 0xFF;
        if (!save(rules)) {
            return false;
        }
    }

    LittleFS.remove(kLegacyPath);
    return true;
}

bool TriggerStorage::save(const piho::TriggerTable &rules) {
    if (!mounted_) {
        return false;
    }

    const uint8_t nextSlot = activeSlot_ == 0 ? 1 : 0;
    const char *path = nextSlot == 0 ? kSlotA : kSlotB;
    const uint32_t nextGeneration = generation_ + 1;
    if (!writeSlot(path, rules, nextGeneration)) {
        return false;
    }

    piho::TriggerTable verified;
    uint32_t verifiedGeneration = 0;
    if (!readSlot(path, verified, verifiedGeneration) || verifiedGeneration != nextGeneration ||
        !tablesEqual(rules, verified)) {
        return false;
    }

    generation_ = nextGeneration;
    activeSlot_ = nextSlot;
    return true;
}