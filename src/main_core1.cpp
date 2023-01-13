#include <Arduino.h>
#include "piho.h"
#include "io.h"
#include "global.h"


uint16_t lastPinStatus = 0;
uint32_t lastIOCheck = 0;

void setup1() {
    Serial.println("( ALL) Core 1 Initialized");
}

void loop1() {
    #ifdef IS_INPUT_DEVICE
    // Process GPIO
    if (millis() - lastIOCheck > IO_CHECK_INTERVAL) {
        lastIOCheck = millis();
        uint16_t pinStatus = readGPIO();
        if (lastPinStatus != pinStatus) {
            lastPinStatus = pinStatus;
            piho0.ReportGPIOChange(pinStatus);
            reportGPIO(getAddr(), pinStatus);
        }
    }
    #else

    #endif
}
