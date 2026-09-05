#pragma once

#include <atomic>
#include <cstdint>

#include "can_transport.h"
#include "pico/util/queue.h"

extern "C" {
#include <can2040.h>
}

class Can2040Transport final : public CanTransport {
   public:
    Can2040Transport(uint32_t gpioRx, uint32_t gpioTx, uint32_t bitrate, uint32_t systemClock);

    bool begin() override;
    void poll() override;
    bool trySend(const piho::CanFrame &frame) override;
    bool tryReceive(piho::CanFrame &frame) override;
    CanTransportStats stats() const override;

   private:
    static void irqWrapper();
    static void callbackWrapper(can2040 *bus, uint32_t notification, can2040_msg *message);

    void handleCallback(uint32_t notification, const can2040_msg &message);
    static piho::CanFrame fromDriverFrame(const can2040_msg &message);
    static bool toDriverFrame(const piho::CanFrame &frame, can2040_msg &message);

    static Can2040Transport *activeInstance_;

    uint32_t gpioRx_;
    uint32_t gpioTx_;
    uint32_t bitrate_;
    uint32_t systemClock_;
    can2040 bus_{};
    queue_t rxQueue_{};
    queue_t txQueue_{};
    bool queuesInitialized_ = false;
    bool started_ = false;
    std::atomic<uint32_t> receivedFrames_{0};
    std::atomic<uint32_t> rxDropped_{0};
    std::atomic<uint32_t> txDropped_{0};
    std::atomic<uint32_t> busErrors_{0};
};
