#pragma once

#include <cstdint>

#include "piho/addressing.h"

namespace piho {

struct DebounceUpdate {
    uint16_t state = 0;
    uint16_t changed = 0;
    bool initialized = false;
};

class InputDebouncer {
   public:
    explicit InputDebouncer(uint32_t debounceMilliseconds) : debounceMilliseconds_(debounceMilliseconds) {}

    DebounceUpdate update(uint16_t sample, uint32_t nowMilliseconds);
    void reset();

   private:
    uint32_t debounceMilliseconds_;
    uint32_t candidateSince_[kPinsPerDevice]{};
    uint16_t stableState_ = 0;
    uint16_t candidateState_ = 0;
    bool initialized_ = false;
};

}  // namespace piho
