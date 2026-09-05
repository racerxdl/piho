#pragma once

#include <cstdint>

#include "piho/trigger_table.h"

class TriggerStorage {
   public:
    bool begin(piho::TriggerTable &rules);
    bool save(const piho::TriggerTable &rules);

    uint32_t generation() const { return generation_; }

   private:
    uint32_t generation_ = 0;
    uint8_t activeSlot_ = 0xFF;
    bool mounted_ = false;
};

extern TriggerStorage triggerStorage;
extern piho::TriggerTable triggerRules;