#include <Arduino.h>

#include <cstdint>

#include "can2040_transport.h"
#include "config.h"
#include "global.h"
#include "io.h"
#include "piho.h"
#include "piho/graph_update.h"
#include "storage.h"
#include "uart.h"

#ifndef IS_INPUT_DEVICE
#include "piho/trigger_table.h"
#endif

namespace {

Can2040Transport canTransport(CAN_RXD, CAN_TXD, CAN_BAUD, F_CPU);
PihoController controller(canTransport);
piho::GraphUpdateParticipant graphUpdateParticipant;
piho::GraphUpdateCoordinator graphUpdateCoordinator;
uint32_t lastGraphUpdateRevision = 0;
#ifdef IS_INPUT_DEVICE
constexpr piho::GraphDeviceRole kLocalGraphRole = piho::GraphDeviceRole::Input;
#else
constexpr piho::GraphDeviceRole kLocalGraphRole = piho::GraphDeviceRole::Output;
#endif
#ifdef IS_INPUT_DEVICE
uint32_t lastInputCheck = 0;
#else
piho::TriggerRouter triggerRouter;
#endif

void reportApplicationError(DeviceErrorCode code) {
    signalError();
    sendErrorEvent(code);
}

void onHealthCheck(void *) {
    signalHealthCheck();
}

void onReset(void *) {
    scheduleReset();
}

void onInputState(void *, uint8_t sourceDevice, uint16_t state) {
    sendInputStateEvent(sourceDevice, state);
#ifndef IS_INPUT_DEVICE
    const uint16_t toggleMask = triggerRouter.update(sourceDevice, state, triggerRules);
    if (toggleMask != 0) {
        toggleOutputs(toggleMask);
    }
#endif
}

#ifndef IS_INPUT_DEVICE
void onOutputState(void *, uint16_t state) {
    setOutputState(state);
}

void onSetPin(void *, uint8_t localPin, bool value) {
    setOutputPin(localPin, value);
}

void onSetByte(void *, uint8_t localByte, uint8_t value) {
    setOutputByte(localByte, value);
}

bool persistTriggerChange(const piho::TriggerTable &previous) {
    if (triggerStorage.save(triggerRules)) {
        return true;
    }
    triggerRules = previous;
    reportApplicationError(DeviceErrorCode::Storage);
    return false;
}

bool onUpsertTrigger(void *, const piho::TriggerRule &rule) {
    const piho::TriggerTable previous = triggerRules;
    switch (triggerRules.upsert(rule)) {
        case piho::TriggerUpdateResult::Inserted:
            return persistTriggerChange(previous);
        case piho::TriggerUpdateResult::AlreadyPresent:
            return true;
        case piho::TriggerUpdateResult::Full:
            reportApplicationError(DeviceErrorCode::TriggerTableFull);
            return false;
        case piho::TriggerUpdateResult::Invalid:
            reportApplicationError(DeviceErrorCode::OutOfRange);
            return false;
    }
    return false;
}

bool onRemoveTrigger(void *, const piho::TriggerRule &rule) {
    const piho::TriggerTable previous = triggerRules;
    if (!triggerRules.remove(rule)) {
        return true;
    }
    return persistTriggerChange(previous);
}

bool onClearTriggers(void *) {
    if (triggerRules.size() == 0) {
        return true;
    }
    const piho::TriggerTable previous = triggerRules;
    triggerRules.clear();
    return persistTriggerChange(previous);
}
#endif

bool writeGraphUpdateFrame(void *, const piho::CanFrame &frame) {
    return controller.sendGraphUpdateFrame(frame);
}

void publishTransportStatus() {
    const CanTransportStats stats = controller.transportStats();
    piho::CanFrame drops{};
    piho::CanFrame errors{};
    if (piho::ProtocolCodec::graphTransportDrops(
            controller.deviceId(), stats.rxDropped, stats.txDropped, drops)) {
        controller.sendGraphUpdateFrame(drops);
    }
    if (piho::ProtocolCodec::graphTransportErrors(
            controller.deviceId(), stats.busErrors, errors)) {
        controller.sendGraphUpdateFrame(errors);
    }
}

void onGraphUpdate(void *, const piho::ProtocolMessage &message) {
    const uint32_t now = millis();
    graphUpdateParticipant.handle(message, now);
    graphUpdateCoordinator.handle(message, now);
    if (message.type == piho::MessageType::GraphStatusRequest) {
        publishTransportStatus();
    }
    if (message.type >= piho::MessageType::GraphNodeCapabilities &&
        message.type <= piho::MessageType::GraphTransportErrors) {
        sendGraphNodeStatusEvent(message);
    }
}

void onControllerError(void *, ControllerError error) {
    signalError();
    switch (error) {
        case ControllerError::InvalidFrame:
            sendErrorEvent(DeviceErrorCode::InvalidFrame);
            break;
        case ControllerError::Transport:
            sendErrorEvent(DeviceErrorCode::Transport);
            break;
        case ControllerError::UnsupportedCommand:
            sendErrorEvent(DeviceErrorCode::InvalidCommand);
            break;
        case ControllerError::Application:
            break;
    }
}

PihoCallbacks callbacks() {
    PihoCallbacks result{};
    result.onHealthCheck = onHealthCheck;
    result.onReset = onReset;
    result.onInputState = onInputState;
    result.onError = onControllerError;
    result.onGraphUpdate = onGraphUpdate;
#ifndef IS_INPUT_DEVICE
    result.onOutputState = onOutputState;
    result.onSetPin = onSetPin;
    result.onSetByte = onSetByte;
    result.onUpsertTrigger = onUpsertTrigger;
    result.onRemoveTrigger = onRemoveTrigger;
    result.onClearTriggers = onClearTriggers;
#endif
    return result;
}

#ifdef IS_INPUT_DEVICE
void publishInputState(uint32_t nowMilliseconds) {
    uint16_t state = 0;
    if (!sampleInputs(nowMilliseconds, state)) {
        return;
    }
    sendInputStateEvent(controller.deviceId(), state);
    controller.reportInputState(state);
}
#endif

}  // namespace

void setup() {
    Serial.begin(115200);
    initializeStatusLeds();
    const uint8_t deviceId = initializeDeviceAddress();
    const piho::GraphStoreError graphStoreError = graphStore.begin();
    if (graphStoreError != piho::GraphStoreError::None ||
        graphStore.status().lastError != piho::GraphStoreError::None) {
        reportApplicationError(DeviceErrorCode::Storage);
    }

#ifdef IS_INPUT_DEVICE
    initializeInputs();
#else
    initializeOutputs();
    if (!triggerStorage.begin(triggerRules)) {
        reportApplicationError(DeviceErrorCode::Storage);
    }
#endif

    controller.setCallbacks(callbacks());
    if (!controller.begin(deviceId)) {
        reportApplicationError(DeviceErrorCode::Transport);
    }
    graphUpdateCoordinator.configure(writeGraphUpdateFrame);
    if (!graphUpdateParticipant.begin(deviceId, kLocalGraphRole, graphStore,
                                      writeGraphUpdateFrame, nullptr, millis())) {
        reportApplicationError(DeviceErrorCode::Storage);
    }

#ifdef IS_INPUT_DEVICE
    lastInputCheck = millis();
    publishInputState(lastInputCheck);
#endif
}

void loop() {
    const uint32_t now = millis();
    controller.poll();
    graphUpdateParticipant.service(now);
    graphUpdateCoordinator.service(now);
    handleUART(controller, graphUpdateCoordinator);
#ifdef IS_INPUT_DEVICE
    if (static_cast<uint32_t>(now - lastInputCheck) >= IO_CHECK_INTERVAL_MS) {
        lastInputCheck = now;
        publishInputState(now);
    }
#endif
    const uint32_t graphRevision = graphUpdateCoordinator.revision();
    if (graphRevision != lastGraphUpdateRevision) {
        lastGraphUpdateRevision = graphRevision;
        sendGraphUpdateEvent(graphUpdateCoordinator.status());
    }
    serviceStatus(now);
}