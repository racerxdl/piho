#pragma once

#include <cstdint>

#include "piho.h"
#include "piho/graph_update.h"

enum class DeviceErrorCode : uint8_t {
    InvalidFrame,
    InvalidCommand,
    OutOfRange,
    Transport,
    Storage,
    TriggerTableFull,
    GraphUpdate,
};

enum class DeviceOperation : uint8_t {
    HealthCheck,
    SetPin,
    SetByte,
    Reset,
    UpsertTrigger,
    RemoveTrigger,
    ClearTriggers,
    GraphBegin,
    GraphChunk,
    GraphFinish,
    GraphAbort,
    GraphActivate,
    GraphRollback,
    GraphStatus,
};

void handleUART(PihoController &controller, piho::GraphUpdateCoordinator &graphUpdate);
void sendInputStateEvent(uint8_t device, uint16_t state);
void sendStatusEvent(const PihoController &controller);
void sendErrorEvent(DeviceErrorCode code);
void sendAckEvent(DeviceOperation operation, bool accepted);
void sendGraphUpdateEvent(const piho::GraphGatewayStatus &status);
void sendGraphNodeStatusEvent(const piho::ProtocolMessage &message);