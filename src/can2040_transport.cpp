#include "can2040_transport.h"

#include <algorithm>
#include <cstring>

#include "config.h"
#include "hardware/irq.h"

Can2040Transport *Can2040Transport::activeInstance_ = nullptr;

Can2040Transport::Can2040Transport(uint32_t gpioRx, uint32_t gpioTx, uint32_t bitrate, uint32_t systemClock)
    : gpioRx_(gpioRx), gpioTx_(gpioTx), bitrate_(bitrate), systemClock_(systemClock) {}

bool Can2040Transport::begin() {
    if (started_) {
        return true;
    }
    if (activeInstance_ != nullptr && activeInstance_ != this) {
        return false;
    }
    if (!queuesInitialized_) {
        queue_init(&rxQueue_, sizeof(piho::CanFrame), PIHO_QUEUE_MAX_ITEMS);
        queue_init(&txQueue_, sizeof(piho::CanFrame), PIHO_QUEUE_MAX_ITEMS);
        queuesInitialized_ = true;
    }

    activeInstance_ = this;
    can2040_setup(&bus_, 0);
    can2040_callback_config(&bus_, &Can2040Transport::callbackWrapper);
    irq_set_exclusive_handler(PIO0_IRQ_0, &Can2040Transport::irqWrapper);
    irq_set_priority(PIO0_IRQ_0, 0x40);
    irq_set_enabled(PIO0_IRQ_0, true);
    can2040_start(&bus_, systemClock_, bitrate_, gpioRx_, gpioTx_);
    started_ = true;
    return true;
}

void Can2040Transport::poll() {
    if (!started_) {
        return;
    }

    while (can2040_check_transmit(&bus_)) {
        piho::CanFrame frame{};
        if (!queue_try_remove(&txQueue_, &frame)) {
            return;
        }

        can2040_msg message{};
        if (!toDriverFrame(frame, message) || can2040_transmit(&bus_, &message) < 0) {
            txDropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool Can2040Transport::trySend(const piho::CanFrame &frame) {
    if (!started_ || frame.length > piho::kCanPayloadCapacity || !queue_try_add(&txQueue_, &frame)) {
        txDropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool Can2040Transport::tryReceive(piho::CanFrame &frame) {
    return started_ && queue_try_remove(&rxQueue_, &frame);
}

CanTransportStats Can2040Transport::stats() const {
    return CanTransportStats{
        receivedFrames_.load(std::memory_order_relaxed),
        rxDropped_.load(std::memory_order_relaxed),
        txDropped_.load(std::memory_order_relaxed),
        busErrors_.load(std::memory_order_relaxed),
    };
}

void Can2040Transport::irqWrapper() {
    if (activeInstance_ != nullptr) {
        can2040_pio_irq_handler(&activeInstance_->bus_);
    }
}

void Can2040Transport::callbackWrapper(can2040 *bus, uint32_t notification, can2040_msg *message) {
    if (activeInstance_ == nullptr || bus != &activeInstance_->bus_ || message == nullptr) {
        return;
    }
    activeInstance_->handleCallback(notification, *message);
}

void Can2040Transport::handleCallback(uint32_t notification, const can2040_msg &message) {
    if ((notification & CAN2040_NOTIFY_ERROR) != 0) {
        busErrors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if ((notification & CAN2040_NOTIFY_RX) == 0) {
        return;
    }

    const piho::CanFrame frame = fromDriverFrame(message);
    receivedFrames_.fetch_add(1, std::memory_order_relaxed);
    if (!queue_try_add(&rxQueue_, &frame)) {
        rxDropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

piho::CanFrame Can2040Transport::fromDriverFrame(const can2040_msg &message) {
    piho::CanFrame frame{};
    frame.identifier = message.id & piho::kCanIdentifierMask;
    frame.extended = (message.id & CAN2040_ID_EFF) != 0;
    frame.remote = (message.id & CAN2040_ID_RTR) != 0;
    frame.length = static_cast<uint8_t>(std::min<uint32_t>(message.dlc, piho::kCanPayloadCapacity));
    std::memcpy(frame.data, message.data, frame.length);
    return frame;
}

bool Can2040Transport::toDriverFrame(const piho::CanFrame &frame, can2040_msg &message) {
    if (frame.length > piho::kCanPayloadCapacity || (frame.identifier & ~piho::kCanIdentifierMask) != 0) {
        return false;
    }

    message = can2040_msg{};
    message.id = frame.identifier;
    if (frame.extended) {
        message.id |= CAN2040_ID_EFF;
    }
    if (frame.remote) {
        message.id |= CAN2040_ID_RTR;
    }
    message.dlc = frame.length;
    std::memcpy(message.data, frame.data, frame.length);
    return true;
}
