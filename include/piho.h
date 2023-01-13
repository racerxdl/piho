#pragma once

#include <Arduino.h>

#include <cstdint>
#include <functional>

#include "pico/stdlib.h"

extern "C" {
#include <can2040.h>
}

enum MessageType {
    INVAL = 0,
    TXT = 1,
    HEALTH_CHECK = 2,
    RESET = 3,
    IO_OUT = 4,
    IO_IN = 5,
    SET_PIN = 6,
    SET_BYTE = 7
};

class PihoController {
    uint32_t gpioRX, gpioTX, bitrate;
    can2040 bus;
    queue_t rxMsgQueue, txMsgQueue;
    can2040_msg rxMsg, txMsg{};
    uint8_t myId;
    std::function<void()> onHealthCheck;
    std::function<void()> onError;
    std::function<void(uint32_t)> onGPIOSet;
    std::function<void(uint16_t, uint8_t)> onSetPin;
    std::function<void(uint16_t, uint8_t)> onSetByte;

    // Handler callback functions
    void CANHandler(can2040 *bus, uint32_t notify, can2040_msg *msg);
    void IRQHandler();

    // State change functions
    void checkTX();
    void checkRX();

   public:
    PihoController(uint32_t gpioRX, uint32_t gpioTX, uint32_t bitrate = 250000);
    PihoController() : PihoController(-1, -1, 0) {}

    void Start();
    void Loop();
    void SetID(uint8_t id) { this->myId = id; }
    void setOnHealthCheck(std::function<void()> cb) { this->onHealthCheck = cb; }
    void setOnError(std::function<void()> cb) { this->onError = cb; }
    void setOnGPIOSet(std::function<void(uint32_t)> cb) { this->onGPIOSet = cb; }
    void setOnPinSet(std::function<void(uint16_t, uint8_t)> cb) { this->onSetPin = cb; }
    void setOnByteSet(std::function<void(uint16_t, uint8_t)> cb) { this->onSetByte = cb; }
    void setError();

    void ReportGPIOChange(uint32_t gpio);
    void SetGPIO(uint8_t devId, uint32_t gpio);
    void SetPin(uint16_t pin, uint8_t value);
    void SetByte(uint16_t byte, uint8_t value);
    void BroadcastHealthCheck();
};

extern PihoController piho0;
