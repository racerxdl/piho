#include <Arduino.h>
#include "global.h"
#include "config.h"

uint16_t pinNumStart, pinNumEnd, byteNumStart, byteNumEnd, myId;
uint32_t lastHC = 0;
uint32_t lastError = 0;

uint8_t getAddr() {
    return (digitalRead(ADDR_BIT_0) << 0) |
           (digitalRead(ADDR_BIT_1) << 1) |
           (digitalRead(ADDR_BIT_2) << 2) |
           (digitalRead(ADDR_BIT_3) << 3) |
           (digitalRead(ADDR_BIT_4) << 4);
}

void initPins() {
    myId = getAddr();
    pinNumStart = static_cast<uint16_t>(myId) * 16;
    pinNumEnd = (static_cast<uint16_t>(myId)+1) * 16;
    byteNumStart = pinNumStart / 8;
    byteNumEnd = pinNumEnd / 8;

    Serial.printf("( ALL) Pin Range: %02d -> %02d\n", pinNumStart, pinNumEnd);
    Serial.printf("( ALL) Byte Range: %02d -> %02d\n", byteNumStart, byteNumEnd);
}

bool pinInDevice(uint16_t pin) {
    return static_cast<uint16_t>(pin) >= pinNumStart && static_cast<uint16_t>(pin) < pinNumEnd;
}

bool byteInDevice(uint16_t byteNum) {
    return byteNum >= byteNumStart && byteNum < byteNumEnd;
}

void healthCheckAction() {
    digitalWrite(LED_HC, HIGH);
    lastHC = millis();
    Serial.println("(  HC) Health Check OK");
}

void checkTimeouts() {
    if (millis() - lastHC > HC_LED_TIMEOUT) {
       digitalWrite(LED_HC, LOW);
    }

    if (millis() - lastError > ERR_LED_TIMEOUT) {
       digitalWrite(LED_ERR, LOW);
    }
}

void onError() {
    digitalWrite(LED_ERR, HIGH);
    lastError = millis();
}