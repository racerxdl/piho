#include "piho.h"

#include "piho/addressing.h"

bool PihoController::begin(uint8_t deviceId) {
    if (!piho::isPhysicalDevice(deviceId)) {
        return false;
    }
    deviceId_ = deviceId;
    started_ = transport_.begin();
    lastStats_ = transport_.stats();
    return started_;
}

void PihoController::poll() {
    if (!started_) {
        return;
    }

    transport_.poll();
    piho::CanFrame frame{};
    while (transport_.tryReceive(frame)) {
        handle(frame);
    }

    const CanTransportStats current = transport_.stats();
    if (current.rxDropped != lastStats_.rxDropped || current.txDropped != lastStats_.txDropped ||
        current.busErrors != lastStats_.busErrors) {
        reportError(ControllerError::Transport);
    }
    lastStats_ = current;
}

bool PihoController::reportInputState(uint16_t state) {
    piho::CanFrame frame{};
    return piho::ProtocolCodec::inputState(deviceId_, state, frame) && send(frame);
}

bool PihoController::setOutputState(uint8_t targetDevice, uint16_t state) {
    if (!piho::isPhysicalDevice(targetDevice)) {
        return false;
    }
    if (targetDevice == deviceId_) {
        if (callbacks_.onOutputState == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        callbacks_.onOutputState(callbacks_.context, state);
        return true;
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::outputState(targetDevice, state, frame) && send(frame);
}

bool PihoController::setGlobalPin(uint16_t globalPin, bool value) {
    piho::PinAddress address{};
    if (!piho::decodeGlobalPin(globalPin, address)) {
        return false;
    }
    if (address.device == deviceId_) {
        if (callbacks_.onSetPin == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        callbacks_.onSetPin(callbacks_.context, address.localPin, value);
        return true;
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::setPin(address.device, address.localPin, value, frame) && send(frame);
}

bool PihoController::setGlobalByte(uint16_t globalByte, uint8_t value) {
    piho::ByteAddress address{};
    if (!piho::decodeGlobalByte(globalByte, address)) {
        return false;
    }
    if (address.device == deviceId_) {
        if (callbacks_.onSetByte == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        callbacks_.onSetByte(callbacks_.context, address.localByte, value);
        return true;
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::setByte(address.device, address.localByte, value, frame) && send(frame);
}

bool PihoController::broadcastHealthCheck() {
    if (callbacks_.onHealthCheck != nullptr) {
        callbacks_.onHealthCheck(callbacks_.context);
    }
    return send(piho::ProtocolCodec::healthCheck());
}

bool PihoController::requestReset(uint8_t device, bool broadcast) {
    const uint8_t target = broadcast ? piho::kBroadcastDevice : device;
    if (!broadcast && !piho::isPhysicalDevice(target)) {
        return false;
    }

    bool sent = true;
    if (broadcast || target != deviceId_) {
        sent = send(piho::ProtocolCodec::reset(target));
    }
    if (isAddressedToThisDevice(target) && callbacks_.onReset != nullptr) {
        callbacks_.onReset(callbacks_.context);
    }
    return sent;
}

bool PihoController::upsertTrigger(uint8_t outputDevice, const piho::TriggerRule &rule) {
    if (!piho::isPhysicalDevice(outputDevice) || !piho::isValid(rule)) {
        return false;
    }
    if (outputDevice == deviceId_) {
        if (callbacks_.onUpsertTrigger == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        return callbacks_.onUpsertTrigger(callbacks_.context, rule);
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::upsertTrigger(outputDevice, rule, frame) && send(frame);
}

bool PihoController::removeTrigger(uint8_t outputDevice, const piho::TriggerRule &rule) {
    if (!piho::isPhysicalDevice(outputDevice) || !piho::isValid(rule)) {
        return false;
    }
    if (outputDevice == deviceId_) {
        if (callbacks_.onRemoveTrigger == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        return callbacks_.onRemoveTrigger(callbacks_.context, rule);
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::removeTrigger(outputDevice, rule, frame) && send(frame);
}

bool PihoController::clearTriggers(uint8_t outputDevice) {
    if (!piho::isPhysicalDevice(outputDevice)) {
        return false;
    }
    if (outputDevice == deviceId_) {
        if (callbacks_.onClearTriggers == nullptr) {
            reportError(ControllerError::UnsupportedCommand);
            return false;
        }
        return callbacks_.onClearTriggers(callbacks_.context);
    }

    piho::CanFrame frame{};
    return piho::ProtocolCodec::clearTriggers(outputDevice, frame) && send(frame);
}

bool PihoController::executeAction(const piho::ActionRequest &request) {
    if (request.sourceDevice != deviceId_) {
        return false;
    }
    piho::CanFrame frame{};
    return piho::ProtocolCodec::executeAction(request, frame) && send(frame);
}

bool PihoController::acknowledgeAction(
    const piho::ActionAcknowledgement &acknowledgement) {
    if (acknowledgement.outputDevice != deviceId_) {
        return false;
    }
    piho::CanFrame frame{};
    return piho::ProtocolCodec::actionAcknowledgement(acknowledgement, frame) && send(frame);
}

bool PihoController::send(const piho::CanFrame &frame) {
    if (started_ && transport_.trySend(frame)) {
        return true;
    }
    lastStats_.txDropped = transport_.stats().txDropped;
    reportError(ControllerError::Transport);
    return false;
}

void PihoController::handle(const piho::CanFrame &frame) {
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    if (!decoded.ok()) {
        if (decoded.error != piho::ProtocolError::NotExtended &&
            decoded.error != piho::ProtocolError::InvalidIdentifier) {
            reportError(ControllerError::InvalidFrame);
        }
        return;
    }

    const piho::ProtocolMessage &message = decoded.message;
    if (message.type == piho::MessageType::InputState) {
        if (callbacks_.onInputState != nullptr) {
            callbacks_.onInputState(callbacks_.context, message.device, message.state);
        }
        return;
    }
    if (!isAddressedToThisDevice(message.device)) {
        return;
    }

    switch (message.type) {
        case piho::MessageType::HealthCheck:
            if (callbacks_.onHealthCheck != nullptr) {
                callbacks_.onHealthCheck(callbacks_.context);
            }
            break;
        case piho::MessageType::Reset:
            if (callbacks_.onReset != nullptr) {
                callbacks_.onReset(callbacks_.context);
            }
            break;
        case piho::MessageType::OutputState:
            if (callbacks_.onOutputState == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else {
                callbacks_.onOutputState(callbacks_.context, message.state);
            }
            break;
        case piho::MessageType::SetPin:
            if (callbacks_.onSetPin == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else {
                callbacks_.onSetPin(callbacks_.context, message.index, message.value != 0);
            }
            break;
        case piho::MessageType::SetByte:
            if (callbacks_.onSetByte == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else {
                callbacks_.onSetByte(callbacks_.context, message.index, message.value);
            }
            break;
        case piho::MessageType::UpsertTrigger:
            if (callbacks_.onUpsertTrigger == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else if (!callbacks_.onUpsertTrigger(callbacks_.context, message.trigger)) {
                reportError(ControllerError::Application);
            }
            break;
        case piho::MessageType::RemoveTrigger:
            if (callbacks_.onRemoveTrigger == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else if (!callbacks_.onRemoveTrigger(callbacks_.context, message.trigger)) {
                reportError(ControllerError::Application);
            }
            break;
        case piho::MessageType::ClearTriggers:
            if (callbacks_.onClearTriggers == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else if (!callbacks_.onClearTriggers(callbacks_.context)) {
                reportError(ControllerError::Application);
            }
            break;
        case piho::MessageType::ExecuteAction:
            if (callbacks_.onExecuteAction == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else {
                callbacks_.onExecuteAction(callbacks_.context, message.actionRequest);
            }
            break;
        case piho::MessageType::ActionAck:
            if (callbacks_.onActionAcknowledgement == nullptr) {
                reportError(ControllerError::UnsupportedCommand);
            } else {
                callbacks_.onActionAcknowledgement(callbacks_.context,
                                                   message.actionAcknowledgement);
            }
            break;
        case piho::MessageType::InputState:
            break;
    }
}

void PihoController::reportError(ControllerError error) {
    if (callbacks_.onError != nullptr) {
        callbacks_.onError(callbacks_.context, error);
    }
}

bool PihoController::isAddressedToThisDevice(uint8_t device) const {
    return device == deviceId_ || device == piho::kBroadcastDevice;
}