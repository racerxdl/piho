#include "global.h"

#include <Arduino.h>

#include "config.h"

namespace {

uint32_t lastHealthCheck = 0;
uint32_t lastError = 0;
uint32_t resetAt = 0;
bool resetPending = false;

}  // namespace

uint8_t initializeDeviceAddress() {
    pinMode(ADDR_BIT_0, INPUT_PULLUP);
    pinMode(ADDR_BIT_1, INPUT_PULLUP);
    pinMode(ADDR_BIT_2, INPUT_PULLUP);
    pinMode(ADDR_BIT_3, INPUT_PULLUP);
    pinMode(ADDR_BIT_4, INPUT_PULLUP);

    return static_cast<uint8_t>((digitalRead(ADDR_BIT_0) << 0) | (digitalRead(ADDR_BIT_1) << 1) |
                                (digitalRead(ADDR_BIT_2) << 2) | (digitalRead(ADDR_BIT_3) << 3) |
                                (digitalRead(ADDR_BIT_4) << 4));
}

void initializeStatusLeds() {
    pinMode(LED_HC, OUTPUT);
    pinMode(LED_ACT, OUTPUT);
    pinMode(LED_ERR, OUTPUT);
    digitalWrite(LED_HC, LOW);
    digitalWrite(LED_ERR, LOW);
    digitalWrite(LED_ACT, HIGH);
}

void signalHealthCheck() {
    digitalWrite(LED_HC, HIGH);
    lastHealthCheck = millis();
}

void signalError() {
    digitalWrite(LED_ERR, HIGH);
    lastError = millis();
}

void scheduleReset() {
    resetAt = millis() + RESET_DELAY_MS;
    resetPending = true;
}

void serviceStatus(uint32_t nowMilliseconds) {
    if (static_cast<uint32_t>(nowMilliseconds - lastHealthCheck) > HC_LED_TIMEOUT_MS) {
        digitalWrite(LED_HC, LOW);
    }
    if (static_cast<uint32_t>(nowMilliseconds - lastError) > ERR_LED_TIMEOUT_MS) {
        digitalWrite(LED_ERR, LOW);
    }
    if (resetPending && static_cast<int32_t>(nowMilliseconds - resetAt) >= 0) {
        watchdog_reboot(0, 0, 0);
    }
}