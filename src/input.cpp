#include <Arduino.h>
#include "piho.h"
#include "config.h"

#ifdef IS_INPUT_DEVICE

uint32_t lastDebounceTime[16];
uint16_t lastValue = 0;
uint16_t debouncedValue = 0;
uint32_t lastLow[16];

void initInputs() {
    gpio_set_dir_in_masked(DATA_MASK);
    for (int i = 0; i < 16; i++) {
        lastDebounceTime[i] = 0;
        lastLow[i] = millis();
        pinMode(i, INPUT_PULLUP);
    }
}

uint16_t readGPIO() {
    uint16_t currentGPIO = (gpio_get_all() & DATA_MASK) >> DATA_SHIFT;
    debouncedValue = 0;
    for (int i = 0; i < 16; i++) {
        // uint16_t lastVal = BIT(lastValue, i);
        uint16_t currVal = BIT(currentGPIO, i);


        if (millis() - lastLow[i] > DEBOUNCE_TIME) {
            debouncedValue = CLRBIT(debouncedValue, i);
        }

        if (currVal) {
            debouncedValue = SETBIT(debouncedValue, i);
        } else {
            lastLow[i] = millis();
        }

        // if (lastVal != currVal) {
        //     lastDebounceTime[i] = millis();
        // }

        // if (millis() - lastDebounceTime[i] > DEBOUNCE_TIME) {
        //     if (currVal) {
        //         debouncedValue = SETBIT(debouncedValue, i);
        //     } else {
        //         debouncedValue = CLRBIT(debouncedValue, i);
        //     }
        // }
    }
    // lastValue = currentGPIO;
    return ~debouncedValue; // debouncedValue;
}

void reportGPIO(uint8_t myId, uint16_t gpio) {
    Serial.printf("(GPIO)%d-0-%d\r\n", myId, (gpio & 0x00FF) >> 0);
    Serial.printf("(GPIO)%d-1-%d\r\n", myId, (gpio & 0xFF00) >> 8);
}

#endif