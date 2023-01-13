#pragma once

#include "config.h"
#include <cstdint>

#ifdef IS_INPUT_DEVICE
void initInputs();
uint16_t readGPIO();
void reportGPIO(uint8_t myId, uint16_t gpio);
#else
void initOutput();
void setGPIOValue(uint32_t v);
void setGPIO(int byteNum, uint8_t value);
void setPin(uint8_t pin, uint8_t val);
#endif