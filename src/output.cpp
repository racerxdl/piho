#include <Arduino.h>
#include "piho.h"
#include "config.h"
#include "io.h"

#ifndef IS_INPUT_DEVICE

uint32_t lastValue = 0;

void initOutput() {
    gpio_set_dir_out_masked(DATA_MASK);
    for (int i = 0; i < 16; i++) {
        pinMode(i, OUTPUT);
    }
}

void togglePin(uint8_t pin) {
    if (BIT(lastValue, pin)) {
        CLRBIT(lastValue, pin);
    } else {
        SETBIT(lastValue, pin);
    }
    setGPIOValue(lastValue);
}

void setGPIOValue(uint32_t v) {
    lastValue = v;
    gpio_put_masked(DATA_MASK, lastValue);
}

void setGPIO(int byteNum, uint8_t value) {
    if (byteNum) {
        lastValue = (lastValue & 0x00FF) | static_cast<uint16_t>(value) << 8;
    } else {
        lastValue = (lastValue & 0xFF00) | value;
    }
    gpio_put_masked(DATA_MASK, lastValue);
}

void setPin(uint8_t pin, uint8_t val) {
    if (val) {
        lastValue = SETBIT(lastValue, pin);
    } else {
        lastValue = CLRBIT(lastValue, pin);
    }
    gpio_put_masked(DATA_MASK, lastValue);
}
#endif