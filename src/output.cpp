#include "io.h"

#include <Arduino.h>

#include "config.h"
#include "piho/addressing.h"

#ifndef IS_INPUT_DEVICE
namespace {

uint16_t logicalState = 0;

uint16_t physicalState(uint16_t state) {
    return OUTPUT_ACTIVE_LOW ? static_cast<uint16_t>(~state) : state;
}

void writeOutputs() {
    const uint32_t shifted = static_cast<uint32_t>(physicalState(logicalState)) << DATA_SHIFT;
    gpio_put_masked(DATA_MASK, shifted & DATA_MASK);
}

}  // namespace

void initializeOutputs() {
    const bool inactiveLevel = OUTPUT_ACTIVE_LOW;
    for (uint8_t pin = 0; pin < piho::kPinsPerDevice; ++pin) {
        const uint8_t gpio = static_cast<uint8_t>(DATA_SHIFT + pin);
        gpio_init(gpio);
        gpio_put(gpio, inactiveLevel);
        gpio_set_dir(gpio, GPIO_OUT);
    }
    logicalState = 0;
    writeOutputs();
}

uint16_t outputState() {
    return logicalState;
}

void setOutputState(uint16_t state) {
    logicalState = state;
    writeOutputs();
}

void setOutputPin(uint8_t localPin, bool value) {
    if (localPin >= piho::kPinsPerDevice) {
        return;
    }
    const uint16_t mask = static_cast<uint16_t>(1u << localPin);
    logicalState = value ? static_cast<uint16_t>(logicalState | mask)
                         : static_cast<uint16_t>(logicalState & static_cast<uint16_t>(~mask));
    writeOutputs();
}

void setOutputByte(uint8_t localByte, uint8_t value) {
    if (localByte >= piho::kBytesPerDevice) {
        return;
    }
    const uint8_t shift = static_cast<uint8_t>(localByte * 8);
    const uint16_t mask = static_cast<uint16_t>(0xFFu << shift);
    logicalState = static_cast<uint16_t>((logicalState & static_cast<uint16_t>(~mask)) |
                                         static_cast<uint16_t>(static_cast<uint16_t>(value) << shift));
    writeOutputs();
}

void toggleOutputs(uint16_t mask) {
    logicalState = static_cast<uint16_t>(logicalState ^ mask);
    writeOutputs();
}

#endif