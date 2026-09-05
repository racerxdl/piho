#include "piho/debouncer.h"

namespace piho {

InputDebouncer::InputDebouncer(uint16_t debounceMilliseconds) {
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        debounceMilliseconds_[pin] = debounceMilliseconds;
    }
}

bool InputDebouncer::setDebounceMilliseconds(uint8_t pin,
                                             uint16_t debounceMilliseconds) {
    if (pin >= kPinsPerDevice) {
        return false;
    }
    debounceMilliseconds_[pin] = debounceMilliseconds;
    return true;
}

DebounceUpdate InputDebouncer::update(uint16_t sample,
                                      uint32_t nowMilliseconds) {
    if (!initialized_) {
        stableState_ = sample;
        candidateState_ = sample;
        for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
            candidateSince_[pin] = nowMilliseconds;
        }
        initialized_ = true;
        return DebounceUpdate{stableState_, 0, 0, 0, true};
    }

    uint16_t changed = 0;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        const uint16_t mask = static_cast<uint16_t>(1u << pin);
        const bool sampled = (sample & mask) != 0;
        const bool candidate = (candidateState_ & mask) != 0;
        const bool stable = (stableState_ & mask) != 0;

        if (sampled != candidate) {
            candidateState_ =
                sampled ? static_cast<uint16_t>(candidateState_ | mask)
                        : static_cast<uint16_t>(
                              candidateState_ & static_cast<uint16_t>(~mask));
            candidateSince_[pin] = nowMilliseconds;
        }

        const bool currentCandidate = (candidateState_ & mask) != 0;
        if (currentCandidate == stable ||
            static_cast<uint32_t>(nowMilliseconds - candidateSince_[pin]) <
                debounceMilliseconds_[pin]) {
            continue;
        }

        stableState_ =
            currentCandidate
                ? static_cast<uint16_t>(stableState_ | mask)
                : static_cast<uint16_t>(
                      stableState_ & static_cast<uint16_t>(~mask));
        changed = static_cast<uint16_t>(changed | mask);
    }

    const uint16_t rising = static_cast<uint16_t>(changed & stableState_);
    const uint16_t falling =
        static_cast<uint16_t>(changed & static_cast<uint16_t>(~stableState_));
    return DebounceUpdate{stableState_, changed, rising, falling, false};
}

void InputDebouncer::reset() {
    stableState_ = 0;
    candidateState_ = 0;
    initialized_ = false;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        candidateSince_[pin] = 0;
    }
}

void InputDebouncer::reset(uint16_t stableState, uint32_t nowMilliseconds) {
    stableState_ = stableState;
    candidateState_ = stableState;
    initialized_ = true;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        candidateSince_[pin] = nowMilliseconds;
    }
}

}  // namespace piho
