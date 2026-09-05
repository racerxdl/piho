#pragma once

#include <cstdint>

#include "can_transport.h"
#include "piho/protocol.h"

enum class ControllerError : uint8_t {
    InvalidFrame,
    Transport,
    UnsupportedCommand,
    Application,
};

struct PihoCallbacks {
    void *context = nullptr;
    void (*onHealthCheck)(void *context) = nullptr;
    void (*onReset)(void *context) = nullptr;
    void (*onInputState)(void *context, uint8_t sourceDevice, uint16_t state) = nullptr;
    void (*onOutputState)(void *context, uint16_t state) = nullptr;
    void (*onSetPin)(void *context, uint8_t localPin, bool value) = nullptr;
    void (*onSetByte)(void *context, uint8_t localByte, uint8_t value) = nullptr;
    bool (*onUpsertTrigger)(void *context, const piho::TriggerRule &rule) = nullptr;
    bool (*onRemoveTrigger)(void *context, const piho::TriggerRule &rule) = nullptr;
    bool (*onClearTriggers)(void *context) = nullptr;
    void (*onError)(void *context, ControllerError error) = nullptr;
};

class PihoController {
   public:
    explicit PihoController(CanTransport &transport) : transport_(transport) {}

    bool begin(uint8_t deviceId);
    void poll();
    void setCallbacks(const PihoCallbacks &callbacks) { callbacks_ = callbacks; }

    bool reportInputState(uint16_t state);
    bool setOutputState(uint8_t targetDevice, uint16_t state);
    bool setGlobalPin(uint16_t globalPin, bool value);
    bool setGlobalByte(uint16_t globalByte, uint8_t value);
    bool broadcastHealthCheck();
    bool requestReset(uint8_t device, bool broadcast);
    bool upsertTrigger(uint8_t outputDevice, const piho::TriggerRule &rule);
    bool removeTrigger(uint8_t outputDevice, const piho::TriggerRule &rule);
    bool clearTriggers(uint8_t outputDevice);

    uint8_t deviceId() const { return deviceId_; }
    CanTransportStats transportStats() const { return transport_.stats(); }

   private:
    bool send(const piho::CanFrame &frame);
    void handle(const piho::CanFrame &frame);
    void reportError(ControllerError error);
    bool isAddressedToThisDevice(uint8_t device) const;

    CanTransport &transport_;
    PihoCallbacks callbacks_{};
    CanTransportStats lastStats_{};
    uint8_t deviceId_ = 0;
    bool started_ = false;
};
