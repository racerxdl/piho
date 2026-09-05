#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/addressing.h"

namespace piho {

struct TriggerRule {
    uint8_t inputDevice = 0;
    uint8_t inputPin = 0;
    uint8_t outputPin = 0;
};

constexpr bool operator==(const TriggerRule &lhs, const TriggerRule &rhs) {
    return lhs.inputDevice == rhs.inputDevice && lhs.inputPin == rhs.inputPin && lhs.outputPin == rhs.outputPin;
}

constexpr bool isValid(const TriggerRule &rule) {
    return isPhysicalDevice(rule.inputDevice) && rule.inputPin < kPinsPerDevice && rule.outputPin < kPinsPerDevice;
}

enum class TriggerUpdateResult : uint8_t {
    Inserted,
    AlreadyPresent,
    Full,
    Invalid,
};

class TriggerTable {
   public:
    static constexpr std::size_t kCapacity = 128;

    TriggerUpdateResult upsert(const TriggerRule &rule);
    bool remove(const TriggerRule &rule);
    void clear();

    std::size_t size() const { return size_; }
    const TriggerRule &at(std::size_t index) const { return rules_[index]; }
    uint16_t toggleMask(uint8_t inputDevice, uint16_t risingInputs) const;

   private:
    TriggerRule rules_[kCapacity]{};
    std::size_t size_ = 0;
};

class TriggerRouter {
   public:
    uint16_t update(uint8_t inputDevice, uint16_t state, const TriggerTable &rules);
    void reset();

   private:
    uint16_t previousState_[kDeviceCount]{};
    uint32_t initializedDevices_ = 0;
};

}  // namespace piho
