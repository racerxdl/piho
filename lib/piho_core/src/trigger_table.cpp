#include "piho/trigger_table.h"

namespace piho {

TriggerUpdateResult TriggerTable::upsert(const TriggerRule &rule) {
    if (!isValid(rule)) {
        return TriggerUpdateResult::Invalid;
    }
    for (std::size_t index = 0; index < size_; ++index) {
        if (rules_[index] == rule) {
            return TriggerUpdateResult::AlreadyPresent;
        }
    }
    if (size_ == kCapacity) {
        return TriggerUpdateResult::Full;
    }
    rules_[size_++] = rule;
    return TriggerUpdateResult::Inserted;
}

bool TriggerTable::remove(const TriggerRule &rule) {
    for (std::size_t index = 0; index < size_; ++index) {
        if (!(rules_[index] == rule)) {
            continue;
        }
        for (std::size_t moveIndex = index + 1; moveIndex < size_; ++moveIndex) {
            rules_[moveIndex - 1] = rules_[moveIndex];
        }
        --size_;
        return true;
    }
    return false;
}

void TriggerTable::clear() {
    size_ = 0;
}

uint16_t TriggerTable::toggleMask(uint8_t inputDevice, uint16_t risingInputs) const {
    if (!isPhysicalDevice(inputDevice)) {
        return 0;
    }

    uint16_t outputs = 0;
    for (std::size_t index = 0; index < size_; ++index) {
        const TriggerRule &rule = rules_[index];
        const uint16_t inputMask = static_cast<uint16_t>(1u << rule.inputPin);
        if (rule.inputDevice == inputDevice && (risingInputs & inputMask) != 0) {
            outputs = static_cast<uint16_t>(outputs | static_cast<uint16_t>(1u << rule.outputPin));
        }
    }
    return outputs;
}

uint16_t TriggerRouter::update(uint8_t inputDevice, uint16_t state, const TriggerTable &rules) {
    if (!isPhysicalDevice(inputDevice)) {
        return 0;
    }

    const uint32_t deviceMask = 1u << inputDevice;
    if ((initializedDevices_ & deviceMask) == 0) {
        initializedDevices_ |= deviceMask;
        previousState_[inputDevice] = state;
        return 0;
    }

    const uint16_t risingInputs = static_cast<uint16_t>(state & static_cast<uint16_t>(~previousState_[inputDevice]));
    previousState_[inputDevice] = state;
    return rules.toggleMask(inputDevice, risingInputs);
}

void TriggerRouter::reset() {
    initializedDevices_ = 0;
    for (uint8_t device = 0; device < kDeviceCount; ++device) {
        previousState_[device] = 0;
    }
}

}  // namespace piho
