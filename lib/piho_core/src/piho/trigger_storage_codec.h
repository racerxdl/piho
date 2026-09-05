#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/trigger_table.h"

namespace piho {

constexpr std::size_t kTriggerStorageCapacity = 12 + TriggerTable::kCapacity * 3 + 4;

enum class TriggerStorageError : uint8_t {
    None,
    BufferTooSmall,
    InvalidMagic,
    UnsupportedVersion,
    InvalidLength,
    InvalidChecksum,
    InvalidRule,
};

class TriggerStorageCodec {
   public:
    static TriggerStorageError encode(const TriggerTable &rules, uint32_t generation, uint8_t *output,
                                      std::size_t outputCapacity, std::size_t &outputSize);
    static TriggerStorageError decode(const uint8_t *data, std::size_t size, TriggerTable &rules,
                                      uint32_t &generation);
};

}  // namespace piho
