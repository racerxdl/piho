#pragma once

#include <cstdint>

#ifdef IS_INPUT_DEVICE
#include "piho/debouncer.h"
#include "piho/graph_image.h"

void initializeInputs();
uint16_t readInputPins();
void configureInputDebounce(const piho::LocalInputGraph &graph,
                            uint16_t baseline, uint32_t nowMilliseconds);
bool sampleInputs(uint32_t nowMilliseconds, piho::DebounceUpdate &update);
uint16_t inputState();
#else
void initializeOutputs();
uint16_t outputState();
void setOutputState(uint16_t state);
void setOutputPin(uint8_t localPin, bool value);
void setOutputByte(uint8_t localByte, uint8_t value);
#endif