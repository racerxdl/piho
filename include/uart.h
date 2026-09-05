#pragma once

#include <cstdint>

#include "piho.h"
#include "piho/graph_update.h"
#include "piho/runtime.h"

enum class DeviceErrorCode : uint8_t {
    InvalidFrame,
    InvalidCommand,
    OutOfRange,
    Transport,
    Storage,
    GraphUpdate,
};

enum class DeviceOperation : uint8_t {
    HealthCheck,
    SetPin,
    SetByte,
    Reset,
    GraphBegin,
    GraphChunk,
    GraphFinish,
    GraphAbort,
    GraphActivate,
    GraphRollback,
    GraphStatus,
};

void handleUART(PihoController &controller,
                piho::GraphUpdateCoordinator &graphUpdate,
                const piho::GraphNodeUpdateStatus &nodeUpdate,
                const piho::GraphRuntimeStatus &runtime);
void sendInputStateEvent(uint8_t device, uint16_t state);
void sendStatusEvent(const PihoController &controller,
                     const piho::GraphNodeUpdateStatus &nodeUpdate,
                     const piho::GraphRuntimeStatus &runtime);
void sendErrorEvent(DeviceErrorCode code);
void sendAckEvent(DeviceOperation operation, bool accepted);
void sendGraphUpdateEvent(const piho::GraphGatewayStatus &status);
void sendGraphNodeStatusEvent(const piho::ProtocolMessage &message);