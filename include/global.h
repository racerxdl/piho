#pragma once

#include <cstdint>

uint8_t initializeDeviceAddress();
void initializeStatusLeds();
void signalHealthCheck();
void signalError();
void scheduleReset();
void serviceStatus(uint32_t nowMilliseconds);