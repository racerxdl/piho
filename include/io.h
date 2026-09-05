#pragma once

#include <cstdint>

#ifdef IS_INPUT_DEVICE
void initializeInputs();
bool sampleInputs(uint32_t nowMilliseconds, uint16_t &state);
uint16_t inputState();
#else
void initializeOutputs();
uint16_t outputState();
void setOutputState(uint16_t state);
void setOutputPin(uint8_t localPin, bool value);
void setOutputByte(uint8_t localByte, uint8_t value);
void toggleOutputs(uint16_t mask);
#endif