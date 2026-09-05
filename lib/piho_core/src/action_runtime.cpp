#include "piho/action_runtime.h"

#include <limits>

namespace piho {

bool OutputActionExecutor::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void OutputActionExecutor::increment(uint32_t &counter) {
    if (counter != std::numeric_limits<uint32_t>::max()) {
        ++counter;
    }
}

OutputActionActivationError OutputActionExecutor::activate(const LocalOutputGraph &graph,
                                                           uint16_t currentOutputs) {
    if (device_ >= kGraphDeviceCapacity) {
        return OutputActionActivationError::InvalidDevice;
    }
    if (graph.identity.format != kGraphFormatVersion ||
        graph.identity.executorApi != kGraphExecutorApiVersion || graph.identity.generation == 0) {
        return OutputActionActivationError::IncompatibleGraph;
    }
    if (graph.device != device_) {
        return OutputActionActivationError::InvalidDevice;
    }
    if (graph.role != GraphDeviceRole::Output) {
        return OutputActionActivationError::RoleMismatch;
    }
    if (graph.actionCount > kGraphLocalActionCapacity) {
        return OutputActionActivationError::InvalidSection;
    }
    for (uint16_t index = 0; index < graph.actionCount; ++index) {
        const GraphActionRecord &action = graph.actions[index];
        if (!isGraphActionValid(action) || action.targetDevice != device_ ||
            graph.findAction(action.id) != &action) {
            return OutputActionActivationError::InvalidSection;
        }
    }

    graph_ = &graph;
    resetRuntime(currentOutputs);
    return OutputActionActivationError::None;
}

void OutputActionExecutor::deactivate() {
    graph_ = nullptr;
    resetRuntime(currentOutputs_);
}

void OutputActionExecutor::resetRuntime(uint16_t currentOutputs) {
    currentOutputs_ = currentOutputs;
    for (DeduplicationEntry &entry : deduplication_) {
        entry.occupied = false;
    }
    for (PulseTimer &timer : pulseTimers_) {
        timer.occupied = false;
    }
    activePulseCount_ = 0;
    counters_ = OutputActionCounters{};
}

ActionAcknowledgement OutputActionExecutor::acknowledgement(
    const ActionRequest &request, ActionAckStatus status) {
    if (status != ActionAckStatus::Executed &&
        status != ActionAckStatus::AlreadyExecuted) {
        increment(counters_.rejected);
    }
    ActionAcknowledgement result{};
    result.generation = request.generation;
    result.eventToken = request.eventToken;
    result.actionId = request.actionId;
    result.sourceDevice = request.sourceDevice;
    result.outputDevice = device_;
    result.status = status;
    return result;
}

bool OutputActionExecutor::writeLogicalPin(uint8_t pin, bool value) {
    const uint16_t mask = static_cast<uint16_t>(1u << pin);
    const bool current = (currentOutputs_ & mask) != 0;
    if (current == value) {
        return true;
    }
    if (writePin_ == nullptr || !writePin_(writeContext_, pin, value)) {
        return false;
    }
    currentOutputs_ = value ? static_cast<uint16_t>(currentOutputs_ | mask)
                            : static_cast<uint16_t>(currentOutputs_ & static_cast<uint16_t>(~mask));
    return true;
}

ActionAcknowledgement OutputActionExecutor::execute(const ActionRequest &request,
                                                     uint32_t nowMilliseconds) {
    if (request.generation == 0 || request.eventToken == 0 ||
        request.eventToken > kActionEventTokenMaximum || request.actionId == 0 ||
        request.actionId > kGraphActionCapacity || !isPhysicalDevice(request.sourceDevice) ||
        !isPhysicalDevice(request.targetDevice)) {
        increment(counters_.invalidActions);
        return acknowledgement(request, ActionAckStatus::InvalidAction);
    }
    if (graph_ == nullptr || writePin_ == nullptr) {
        increment(counters_.unavailableOutputs);
        return acknowledgement(request, ActionAckStatus::UnavailableOutput);
    }
    if (request.generation != graph_->identity.generation) {
        increment(counters_.wrongGenerations);
        return acknowledgement(request, ActionAckStatus::WrongGeneration);
    }
    if (request.targetDevice != device_) {
        increment(counters_.wrongTargets);
        return acknowledgement(request, ActionAckStatus::WrongTarget);
    }
    const GraphActionRecord *action = graph_->findAction(request.actionId);
    if (action == nullptr) {
        increment(counters_.unknownActions);
        return acknowledgement(request, ActionAckStatus::UnknownAction);
    }
    if (!isGraphActionValid(*action)) {
        increment(counters_.invalidActions);
        return acknowledgement(request, ActionAckStatus::InvalidAction);
    }
    if (action->targetDevice != device_) {
        increment(counters_.wrongTargets);
        return acknowledgement(request, ActionAckStatus::WrongTarget);
    }

    std::size_t availableSlot = kActionDeduplicationCapacity;
    for (std::size_t index = 0; index < kActionDeduplicationCapacity; ++index) {
        DeduplicationEntry &entry = deduplication_[index];
        if (entry.occupied && timeReached(nowMilliseconds, entry.expiresAt)) {
            entry.occupied = false;
        }
        if (!entry.occupied) {
            if (availableSlot == kActionDeduplicationCapacity) {
                availableSlot = index;
            }
            continue;
        }
        if (entry.eventToken == request.eventToken && entry.actionId == request.actionId &&
            entry.sourceDevice == request.sourceDevice) {
            increment(counters_.duplicates);
            return acknowledgement(request, ActionAckStatus::AlreadyExecuted);
        }
    }
    if (availableSlot == kActionDeduplicationCapacity) {
        increment(counters_.deduplicationOverflows);
        increment(counters_.unavailableOutputs);
        return acknowledgement(request, ActionAckStatus::UnavailableOutput);
    }

    const uint16_t mask = static_cast<uint16_t>(1u << action->targetPin);
    bool value = false;
    switch (action->operation) {
        case GraphOperation::Set:
            value = action->value;
            break;
        case GraphOperation::CopySource:
            value = request.sourceValue;
            break;
        case GraphOperation::Toggle:
            value = (currentOutputs_ & mask) == 0;
            break;
        case GraphOperation::Pulse:
            value = true;
            break;
    }
    if (!writeLogicalPin(action->targetPin, value)) {
        increment(counters_.unavailableOutputs);
        return acknowledgement(request, ActionAckStatus::UnavailableOutput);
    }

    PulseTimer &timer = pulseTimers_[action->targetPin];
    if (action->operation == GraphOperation::Pulse) {
        if (!timer.occupied) {
            ++activePulseCount_;
        }
        timer.dueAt = nowMilliseconds + action->durationMs;
        timer.occupied = true;
    } else if (timer.occupied) {
        timer.occupied = false;
        --activePulseCount_;
    }

    DeduplicationEntry &entry = deduplication_[availableSlot];
    entry.eventToken = request.eventToken;
    entry.actionId = request.actionId;
    entry.sourceDevice = request.sourceDevice;
    entry.expiresAt = nowMilliseconds + kActionDeduplicationWindowMs;
    entry.occupied = true;
    increment(counters_.executed);
    return acknowledgement(request, ActionAckStatus::Executed);
}

std::size_t OutputActionExecutor::service(uint32_t nowMilliseconds) {
    std::size_t work = 0;
    for (uint8_t pin = 0; pin < kPinsPerDevice && work < kActionRuntimeServiceBudget; ++pin) {
        PulseTimer &timer = pulseTimers_[pin];
        if (!timer.occupied || !timeReached(nowMilliseconds, timer.dueAt)) {
            continue;
        }
        ++work;
        if (!writeLogicalPin(pin, false)) {
            increment(counters_.pulseCompletionFailures);
            increment(counters_.unavailableOutputs);
            continue;
        }
        timer.occupied = false;
        --activePulseCount_;
        increment(counters_.completedPulses);
    }
    return work;
}

bool ReliableActionSender::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void ReliableActionSender::increment(uint32_t &counter) {
    if (counter != std::numeric_limits<uint32_t>::max()) {
        ++counter;
    }
}

bool ReliableActionSender::enqueue(const FlowActionInvocation &invocation,
                                   uint32_t nowMilliseconds) {
    if (!isPhysicalDevice(sourceDevice_) || invocation.generation == 0 ||
        invocation.eventToken == 0 || invocation.eventToken > kActionEventTokenMaximum ||
        invocation.actionId == 0 || invocation.actionId > kGraphActionCapacity ||
        invocation.sourceDevice != sourceDevice_ || !isPhysicalDevice(invocation.targetDevice)) {
        increment(counters_.invalidEnqueues);
        return false;
    }
    for (const PendingAction &entry : pending_) {
        if (entry.occupied && entry.request.generation == invocation.generation &&
            entry.request.eventToken == invocation.eventToken &&
            entry.request.actionId == invocation.actionId) {
            increment(counters_.duplicateEnqueues);
            return false;
        }
    }
    if (pendingCount_ >= kActionPendingCapacity) {
        increment(counters_.queueOverflows);
        return false;
    }
    for (PendingAction &entry : pending_) {
        if (entry.occupied) {
            continue;
        }
        entry.request.generation = invocation.generation;
        entry.request.eventToken = invocation.eventToken;
        entry.request.actionId = invocation.actionId;
        entry.request.sourceDevice = sourceDevice_;
        entry.request.targetDevice = invocation.targetDevice;
        entry.request.sourceValue = invocation.sourceValue;
        entry.retryAt = nowMilliseconds;
        entry.sequence = nextSequence_++;
        entry.expiresAt = nowMilliseconds + kActionRetryLifetimeMs;
        entry.attempts = 0;
        entry.awaitingAcknowledgement = false;
        entry.occupied = true;
        ++pendingCount_;
        increment(counters_.enqueued);
        return true;
    }
    increment(counters_.queueOverflows);
    return false;
}

std::size_t ReliableActionSender::nextDue(uint32_t nowMilliseconds) const {
    std::size_t selected = kActionPendingCapacity;
    uint32_t selectedSequence = 0;
    for (std::size_t index = 0; index < kActionPendingCapacity; ++index) {
        const PendingAction &entry = pending_[index];
        if (!entry.occupied || !timeReached(nowMilliseconds, entry.retryAt)) {
            continue;
        }
        if (selected == kActionPendingCapacity ||
            static_cast<int32_t>(entry.sequence - selectedSequence) < 0) {
            selected = index;
            selectedSequence = entry.sequence;
        }
    }
    return selected;
}

std::size_t ReliableActionSender::service(uint32_t nowMilliseconds) {
    std::size_t work = 0;
    while (work < kActionRuntimeServiceBudget) {
        const std::size_t selected = nextDue(nowMilliseconds);
        if (selected == kActionPendingCapacity) {
            break;
        }
        PendingAction &entry = pending_[selected];
        if (entry.awaitingAcknowledgement) {
            increment(counters_.timeouts);
        }
        if (timeReached(nowMilliseconds, entry.expiresAt)) {
            entry.occupied = false;
            --pendingCount_;
            increment(counters_.exhausted);
            ++work;
            continue;
        }
        if (entry.attempts >= kActionMaximumAttempts) {
            entry.occupied = false;
            --pendingCount_;
            increment(counters_.exhausted);
            ++work;
            continue;
        }

        CanFrame frame{};
        if (!ProtocolCodec::executeAction(entry.request, frame)) {
            entry.occupied = false;
            --pendingCount_;
            increment(counters_.invalidEnqueues);
            ++work;
            continue;
        }
        if (entry.attempts != 0) {
            increment(counters_.retries);
        }
        increment(counters_.transmissionAttempts);
        const bool sent = writeFrame_ != nullptr && writeFrame_(writeContext_, frame);
        if (!sent) {
            increment(counters_.sendFailures);
        }
        entry.awaitingAcknowledgement = sent;
        ++entry.attempts;
        entry.retryAt = nowMilliseconds + kActionAcknowledgementTimeoutMs;
        ++work;
    }
    return work;
}

bool ReliableActionSender::acknowledge(const ActionAcknowledgement &acknowledgement,
                                       uint32_t nowMilliseconds) {
    if (acknowledgement.sourceDevice != sourceDevice_ ||
        !isPhysicalDevice(acknowledgement.outputDevice)) {
        increment(counters_.unexpectedAcknowledgements);
        return false;
    }
    for (PendingAction &entry : pending_) {
        if (!entry.occupied || entry.attempts == 0 ||
            entry.request.generation != acknowledgement.generation ||
            entry.request.eventToken != acknowledgement.eventToken ||
            entry.request.actionId != acknowledgement.actionId ||
            entry.request.targetDevice != acknowledgement.outputDevice) {
            continue;
        }
        switch (acknowledgement.status) {
            case ActionAckStatus::Executed:
                entry.occupied = false;
                --pendingCount_;
                increment(counters_.acknowledged);
                return true;
            case ActionAckStatus::AlreadyExecuted:
                entry.occupied = false;
                --pendingCount_;
                increment(counters_.acknowledged);
                increment(counters_.alreadyExecutedAcknowledgements);
                return true;
            case ActionAckStatus::UnavailableOutput:
                increment(counters_.rejections);
                increment(counters_.unavailableAcknowledgements);
                entry.retryAt = nowMilliseconds + kActionAcknowledgementTimeoutMs;
                entry.awaitingAcknowledgement = false;
                return true;
            case ActionAckStatus::WrongGeneration:
            case ActionAckStatus::UnknownAction:
            case ActionAckStatus::WrongTarget:
            case ActionAckStatus::InvalidAction:
                entry.occupied = false;
                --pendingCount_;
                increment(counters_.rejections);
                return true;
        }
    }
    increment(counters_.unexpectedAcknowledgements);
    return false;
}

void ReliableActionSender::clear() {
    for (PendingAction &entry : pending_) {
        entry.occupied = false;
    }
    pendingCount_ = 0;
}

}  // namespace piho
