#pragma once

#include <cstdint>

uint8_t getAddr();
void initPins();

bool pinInDevice(uint16_t pin);
bool byteInDevice(uint16_t byteNum);
void healthCheckAction();

void checkTimeouts();

extern uint16_t pinNumStart, pinNumEnd, byteNumStart, byteNumEnd, myId;