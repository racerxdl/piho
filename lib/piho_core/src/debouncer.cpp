#include "piho/debouncer.h"

namespace piho {

DebounceUpdate InputDebouncer::update(uint16_t sample, uint32_t nowMilliseconds) {
    if (!initialized_) {
        stableState_ = sample;
        candidateState_ = sample;
        for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
            candidateSince_[pin] = nowMilliseconds;
        }
        initialized_ = true;
        return DebounceUpdate{stableState_, 0, true};
    }

    uint16_t changed = 0;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        const uint16_t mask = static_cast<uint16_t>(1u << pin);
        const bool sampled = (sample & mask) != 0;
        const bool candidate = (candidateState_ & mask) != 0;
        const bool stable = (stableState_ & mask) != 0;

        if (sampled != candidate) {
            candidateState_ = sampled ? static_cast<uint16_t>(candidateState_ | mask)
                                      : static_cast<uint16_t>(candidateState_ & static_cast<uint16_t>(~mask));
            candidateSince_[pin] = nowMilliseconds;
        }

        const bool currentCandidate = (candidateState_ & mask) != 0;
        if (currentCandidate == stable ||
            static_cast<uint32_t>(nowMilliseconds - candidateSince_[pin]) < debounceMilliseconds_) {
            continue;
        }

        stableState_ = currentCandidate ? static_cast<uint16_t>(stableState_ | mask)
                                        : static_cast<uint16_t>(stableState_ & static_cast<uint16_t>(~mask));
        changed = static_cast<uint16_t>(changed | mask);
    }

    return DebounceUpdate{stableState_, changed, false};
}

void InputDebouncer::reset() {
    stableState_ = 0;
    candidateState_ = 0;
    initialized_ = false;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        candidateSince_[pin] = 0;
    }
}

}  // namespace piho
