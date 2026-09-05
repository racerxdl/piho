#pragma once

#include <cstdint>

#include "piho/addressing.h"

namespace piho {

struct DebounceUpdate {
    uint16_t state = 0;
    uint16_t changed = 0;
    uint16_t rising = 0;
    uint16_t falling = 0;
    bool initialized = false;
};

class InputDebouncer {
   public:
    explicit InputDebouncer(uint16_t debounceMilliseconds = 0);

    bool setDebounceMilliseconds(uint8_t pin, uint16_t debounceMilliseconds);
    DebounceUpdate update(uint16_t sample, uint32_t nowMilliseconds);
    void reset();
    void reset(uint16_t stableState, uint32_t nowMilliseconds);

   private:
    uint16_t debounceMilliseconds_[kPinsPerDevice]{};
    uint32_t candidateSince_[kPinsPerDevice]{};
    uint16_t stableState_ = 0;
    uint16_t candidateState_ = 0;
    bool initialized_ = false;
};

}  // namespace piho
