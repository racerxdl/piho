#include <Arduino.h>

#include <cstdint>
#include <optional>

#include "can2040_transport.h"
#include "config.h"
#include "global.h"
#include "io.h"
#include "piho.h"
#include "piho/graph_update.h"
#include "piho/runtime.h"
#include "storage.h"
#include "uart.h"

namespace {

Can2040Transport canTransport(CAN_RXD, CAN_TXD, CAN_BAUD, F_CPU);
PihoController controller(canTransport);
piho::GraphUpdateParticipant graphUpdateParticipant;
piho::GraphUpdateCoordinator graphUpdateCoordinator;
uint32_t lastGraphUpdateRevision = 0;
#ifdef IS_INPUT_DEVICE
constexpr piho::GraphDeviceRole kLocalGraphRole =
    piho::GraphDeviceRole::Input;
std::optional<piho::InputGraphRuntime> graphRuntime;
uint32_t lastInputCheck = 0;
#else
constexpr piho::GraphDeviceRole kLocalGraphRole =
    piho::GraphDeviceRole::Output;
std::optional<piho::OutputGraphRuntime> graphRuntime;
#endif

bool sameIdentity(const piho::GraphIdentity &left,
                  const piho::GraphIdentity &right) {
    return left.generation == right.generation &&
           left.checksum == right.checksum;
}

void reportApplicationError(DeviceErrorCode code) {
    signalError();
    sendErrorEvent(code);
}

piho::GraphRuntimeStatus runtimeStatus() {
    return graphRuntime.has_value() ? graphRuntime->status()
                                    : piho::GraphRuntimeStatus{};
}

void onHealthCheck(void *) {
    signalHealthCheck();
}

void onReset(void *) {
    scheduleReset();
}

void onInputState(void *, uint8_t sourceDevice, uint16_t state) {
    sendInputStateEvent(sourceDevice, state);
}

#ifndef IS_INPUT_DEVICE
void synchronizeOutputRuntime() {
    if (graphRuntime.has_value()) {
        graphRuntime->synchronizeOutputs(outputState());
    }
}

void onOutputState(void *, uint16_t state) {
    setOutputState(state);
    synchronizeOutputRuntime();
}

void onSetPin(void *, uint8_t localPin, bool value) {
    setOutputPin(localPin, value);
    synchronizeOutputRuntime();
}

void onSetByte(void *, uint8_t localByte, uint8_t value) {
    setOutputByte(localByte, value);
    synchronizeOutputRuntime();
}

bool writeOutputPin(void *, uint8_t localPin, bool value) {
    setOutputPin(localPin, value);
    return true;
}
#endif

bool writeGraphUpdateFrame(void *, const piho::CanFrame &frame) {
    return controller.sendGraphUpdateFrame(frame);
}

bool writeActionFrame(void *, const piho::CanFrame &frame) {
    return controller.sendActionFrame(frame);
}

bool applyActiveGraph(void *, const piho::GraphIdentity &identity,
                      uint32_t nowMilliseconds) {
    if (sameIdentity(runtimeStatus().identity, identity)) {
        return true;
    }
#ifdef IS_INPUT_DEVICE
    const uint16_t baseline = readInputPins();
    if (!graphRuntime.has_value() || !graphRuntime->activate(baseline) ||
        graphRuntime->activeGraph() == nullptr) {
        reportApplicationError(DeviceErrorCode::GraphUpdate);
        return false;
    }
    configureInputDebounce(*graphRuntime->activeGraph(), baseline,
                           nowMilliseconds);
    sendInputStateEvent(controller.deviceId(), baseline);
    controller.reportInputState(baseline);
#else
    if (!graphRuntime.has_value() ||
        !graphRuntime->activate(outputState())) {
        reportApplicationError(DeviceErrorCode::GraphUpdate);
        return false;
    }
#endif
    if (!sameIdentity(runtimeStatus().identity, identity)) {
        reportApplicationError(DeviceErrorCode::GraphUpdate);
        return false;
    }
    return true;
}

#ifndef IS_INPUT_DEVICE
void onExecuteAction(void *, const piho::ActionRequest &request) {
    if (!graphRuntime.has_value()) {
        return;
    }
    const piho::ActionAcknowledgement acknowledgement =
        graphRuntime->execute(request, millis());
    controller.acknowledgeAction(acknowledgement);
}
#else
void onActionAcknowledgement(
    void *, const piho::ActionAcknowledgement &acknowledgement) {
    if (graphRuntime.has_value()) {
        graphRuntime->acknowledge(acknowledgement, millis());
    }
}
#endif

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

void publishRuntimeStatus() {
    const piho::GraphRuntimeStatus status = runtimeStatus();
    piho::CanFrame frames[4]{};
    const bool encoded[] = {
        piho::ProtocolCodec::graphRuntimeIdentity(
            controller.deviceId(), status.identity, frames[0]),
        piho::ProtocolCodec::graphFlowCounters(
            controller.deviceId(), status.flowAcceptedEvents,
            status.flowEvaluatedActions, frames[1]),
        piho::ProtocolCodec::graphActionCounters(
            controller.deviceId(), status.actionRetries,
            status.actionRejections, frames[2]),
        piho::ProtocolCodec::graphExecutorCounters(
            controller.deviceId(), status.executorExecutedActions,
            status.executorRejectedActions, frames[3]),
    };
    for (uint8_t index = 0; index < 4; ++index) {
        if (encoded[index]) {
            controller.sendGraphUpdateFrame(frames[index]);
        }
    }
}

void onGraphUpdate(void *, const piho::ProtocolMessage &message) {
    const uint32_t now = millis();
    graphUpdateParticipant.handle(message, now);
    graphUpdateCoordinator.handle(message, now);
    if (message.type == piho::MessageType::GraphStatusRequest) {
        publishTransportStatus();
        publishRuntimeStatus();
    }
    if (message.type >= piho::MessageType::GraphNodeCapabilities &&
        message.type <= piho::MessageType::GraphExecutorCounters) {
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
#ifdef IS_INPUT_DEVICE
    result.onActionAcknowledgement = onActionAcknowledgement;
#else
    result.onOutputState = onOutputState;
    result.onSetPin = onSetPin;
    result.onSetByte = onSetByte;
    result.onExecuteAction = onExecuteAction;
#endif
    return result;
}

#ifdef IS_INPUT_DEVICE
void publishInputState(uint32_t nowMilliseconds) {
    piho::DebounceUpdate update{};
    if (!sampleInputs(nowMilliseconds, update)) {
        return;
    }
    sendInputStateEvent(controller.deviceId(), update.state);
    controller.reportInputState(update.state);
    if (update.changed != 0 && graphRuntime.has_value() &&
        graphRuntime->active()) {
        graphRuntime->submitInput(piho::FlowInputUpdate{
            update.state, update.changed, update.rising, update.falling,
            nowMilliseconds});
    }
}
#endif

void serviceGraphRuntime(uint32_t nowMilliseconds) {
    if (graphRuntime.has_value()) {
        graphRuntime->service(nowMilliseconds);
    }
}

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
#endif

    controller.setCallbacks(callbacks());
    if (!controller.begin(deviceId)) {
        reportApplicationError(DeviceErrorCode::Transport);
    }
#ifdef IS_INPUT_DEVICE
    graphRuntime.emplace(deviceId, graphStore, writeActionFrame, nullptr);
#else
    graphRuntime.emplace(deviceId, graphStore, writeOutputPin, nullptr);
#endif
    graphUpdateCoordinator.configure(writeGraphUpdateFrame);
    const uint32_t now = millis();
    const bool participantStarted = graphUpdateParticipant.begin(
        deviceId, kLocalGraphRole, graphStore, writeGraphUpdateFrame, nullptr,
        now, applyActiveGraph, nullptr);
    if (!participantStarted && !graphStore.hasActiveGraph()) {
        reportApplicationError(DeviceErrorCode::GraphUpdate);
    }

#ifdef IS_INPUT_DEVICE
    lastInputCheck = now;
    if (!graphRuntime->active()) {
        publishInputState(now);
    }
#endif
}

void loop() {
    const uint32_t now = millis();
    controller.poll();
    graphUpdateParticipant.service(now);
    graphUpdateCoordinator.service(now);
    handleUART(controller, graphUpdateCoordinator,
               graphUpdateParticipant.status(), runtimeStatus());
#ifdef IS_INPUT_DEVICE
    if (static_cast<uint32_t>(now - lastInputCheck) >=
        IO_CHECK_INTERVAL_MS) {
        lastInputCheck = now;
        publishInputState(now);
    }
#endif
    serviceGraphRuntime(now);
    const uint32_t graphRevision = graphUpdateCoordinator.revision();
    if (graphRevision != lastGraphUpdateRevision) {
        lastGraphUpdateRevision = graphRevision;
        sendGraphUpdateEvent(graphUpdateCoordinator.status());
    }
    serviceStatus(now);
}