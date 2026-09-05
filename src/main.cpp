#include <Arduino.h>

#include <cstdint>

#include "can2040_transport.h"
#include "config.h"
#include "global.h"
#include "io.h"
#include "piho.h"
#include "storage.h"
#include "uart.h"

#ifndef IS_INPUT_DEVICE
#include "piho/trigger_table.h"
#endif

namespace {

Can2040Transport canTransport(CAN_RXD, CAN_TXD, CAN_BAUD, F_CPU);
PihoController controller(canTransport);
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

#ifdef IS_INPUT_DEVICE
    lastInputCheck = millis();
    publishInputState(lastInputCheck);
#endif
}

void loop() {
    controller.poll();
    handleUART(controller);

    const uint32_t now = millis();
#ifdef IS_INPUT_DEVICE
    if (static_cast<uint32_t>(now - lastInputCheck) >= IO_CHECK_INTERVAL_MS) {
        lastInputCheck = now;
        publishInputState(now);
    }
#endif
    serviceStatus(now);
}