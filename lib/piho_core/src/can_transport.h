#pragma once

#include <cstdint>

#include "piho/can_frame.h"

struct CanTransportStats {
    uint32_t receivedFrames = 0;
    uint32_t rxDropped = 0;
    uint32_t txDropped = 0;
    uint32_t busErrors = 0;
};

class CanTransport {
   public:
    virtual ~CanTransport() = default;

    virtual bool begin() = 0;
    virtual void poll() = 0;
    virtual bool trySend(const piho::CanFrame &frame) = 0;
    virtual bool tryReceive(piho::CanFrame &frame) = 0;
    virtual CanTransportStats stats() const = 0;
};
