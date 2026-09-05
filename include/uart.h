#pragma once

#include <cstdint>

#include "piho.h"

enum class DeviceErrorCode : uint8_t {
    InvalidFrame,
    InvalidCommand,
    OutOfRange,
    Transport,
    Storage,
    TriggerTableFull,
};

enum class DeviceOperation : uint8_t {
    HealthCheck,
    SetPin,
    SetByte,
    Reset,
    UpsertTrigger,
    RemoveTrigger,
    ClearTriggers,
};

void handleUART(PihoController &controller);
void sendInputStateEvent(uint8_t device, uint16_t state);
void sendStatusEvent(const PihoController &controller);
void sendErrorEvent(DeviceErrorCode code);
void sendAckEvent(DeviceOperation operation, bool accepted);