#include "piho/flow_engine.h"

#include <cstring>
#include <limits>

namespace piho {

std::size_t FlowEngine::edgeIndex(GraphEdge edge) {
    switch (edge) {
        case GraphEdge::Rising:
            return 0;
        case GraphEdge::Falling:
            return 1;
        case GraphEdge::Changed:
            return 2;
    }
    return 3;
}

bool FlowEngine::timeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void FlowEngine::increment(uint32_t &counter) {
    if (counter != std::numeric_limits<uint32_t>::max()) {
        ++counter;
    }
}

FlowEngineError FlowEngine::activate(const LocalInputGraph &graph, uint16_t currentInputs,
                                     uint32_t eventTokenSeed) {
    if (device_ >= kGraphDeviceCapacity) {
        return FlowEngineError::InvalidDevice;
    }
    if (graph.identity.format != kGraphFormatVersion ||
        graph.identity.executorApi != kGraphExecutorApiVersion || graph.identity.generation == 0) {
        return FlowEngineError::IncompatibleGraph;
    }
    if (graph.device != device_) {
        return FlowEngineError::InvalidDevice;
    }
    if (graph.role != GraphDeviceRole::Input) {
        return FlowEngineError::RoleMismatch;
    }
    if (eventTokenSeed == 0 || graph.inputCount > kGraphLocalInputCapacity ||
        graph.routeCount > kGraphLocalRouteCapacity ||
        graph.actionReferenceCount > kGraphLocalActionReferenceCapacity ||
        graph.referencedActionCount > kGraphActionCapacity) {
        return FlowEngineError::InvalidSection;
    }

    uint16_t inputIdByPin[kPinsPerDevice]{};
    RouteSpan routeSpans[kPinsPerDevice][3]{};
    for (uint16_t index = 0; index < graph.inputCount; ++index) {
        const GraphInputRecord &input = graph.inputs[index];
        if (input.id == 0 || input.id > kGraphInputCapacity || input.device != device_ ||
            input.pin >= kPinsPerDevice || input.debounceMs > kGraphMaximumDebounceMs ||
            inputIdByPin[input.pin] != 0) {
            return FlowEngineError::InvalidSection;
        }
        inputIdByPin[input.pin] = input.id;
    }

    bool referencedActionIds[kGraphActionCapacity + 1]{};
    for (uint16_t index = 0; index < graph.referencedActionCount; ++index) {
        const GraphActionRecord &action = graph.referencedActions[index];
        const bool validOperation =
            action.operation == GraphOperation::Set || action.operation == GraphOperation::CopySource ||
            action.operation == GraphOperation::Toggle || action.operation == GraphOperation::Pulse;
        const bool validParameters =
            (action.operation == GraphOperation::Pulse &&
             action.durationMs != 0 && action.durationMs <= kGraphMaximumActionTimeMs &&
             !action.value) ||
            (action.operation == GraphOperation::Set && action.durationMs == 0) ||
            ((action.operation == GraphOperation::CopySource ||
              action.operation == GraphOperation::Toggle) &&
             action.durationMs == 0 && !action.value);
        if (action.id == 0 || action.id > kGraphActionCapacity ||
            action.targetDevice >= kGraphDeviceCapacity || action.targetPin >= kPinsPerDevice ||
            !validOperation || !validParameters || action.delayMs > kGraphMaximumActionTimeMs ||
            graph.findAction(action.id) != &action) {
            return FlowEngineError::InvalidSection;
        }
    }

    uint16_t expectedReferenceStart = 0;
    uint8_t actionCountByPinEdge[kPinsPerDevice][3]{};
    for (uint16_t index = 0; index < graph.routeCount; ++index) {
        const LocalGraphRoute &route = graph.routes[index];
        const std::size_t selector = edgeIndex(route.edge);
        if (route.id == 0 || route.flowId == 0 || route.inputId == 0 || selector >= 3 ||
            route.actionCount == 0 || route.actionCount > kGraphActionsPerEvent ||
            route.actionReferenceStart != expectedReferenceStart ||
            static_cast<std::size_t>(route.actionReferenceStart) + route.actionCount >
                graph.actionReferenceCount) {
            return FlowEngineError::InvalidSection;
        }

        uint8_t inputPin = kPinsPerDevice;
        for (uint16_t inputIndex = 0; inputIndex < graph.inputCount; ++inputIndex) {
            if (graph.inputs[inputIndex].id == route.inputId) {
                inputPin = graph.inputs[inputIndex].pin;
                break;
            }
        }
        if (inputPin >= kPinsPerDevice) {
            return FlowEngineError::InvalidSection;
        }

        RouteSpan &span = routeSpans[inputPin][selector];
        if (span.count == 0) {
            span.start = index;
        } else if (static_cast<std::size_t>(span.start) + span.count != index) {
            return FlowEngineError::InvalidSection;
        }
        ++span.count;
        if (actionCountByPinEdge[inputPin][selector] + route.actionCount >
            kGraphActionsPerEvent) {
            return FlowEngineError::InvalidSection;
        }
        actionCountByPinEdge[inputPin][selector] =
            static_cast<uint8_t>(actionCountByPinEdge[inputPin][selector] + route.actionCount);

        uint16_t previousActionId = 0;
        for (uint8_t actionIndex = 0; actionIndex < route.actionCount; ++actionIndex) {
            const uint16_t actionId =
                graph.actionReferences[route.actionReferenceStart + actionIndex];
            if (actionId == 0 || actionId <= previousActionId || graph.findAction(actionId) == nullptr) {
                return FlowEngineError::InvalidSection;
            }
            referencedActionIds[actionId] = true;
            previousActionId = actionId;
        }
        expectedReferenceStart = static_cast<uint16_t>(expectedReferenceStart + route.actionCount);
    }
    if (expectedReferenceStart != graph.actionReferenceCount) {
        return FlowEngineError::InvalidSection;
    }

    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        if (inputIdByPin[pin] == 0) {
            continue;
        }
        if (routeSpans[pin][0].count == 0 && routeSpans[pin][1].count == 0 &&
            routeSpans[pin][2].count == 0) {
            return FlowEngineError::InvalidSection;
        }
        for (std::size_t transition = 0; transition < 2; ++transition) {
            uint16_t eventActions[kGraphActionsPerEvent]{};
            uint8_t eventActionCount = 0;
            const std::size_t selectors[] = {transition, 2};
            for (std::size_t selector : selectors) {
                const RouteSpan &span = routeSpans[pin][selector];
                for (uint16_t routeOffset = 0; routeOffset < span.count; ++routeOffset) {
                    const LocalGraphRoute &route = graph.routes[span.start + routeOffset];
                    for (uint8_t actionOffset = 0; actionOffset < route.actionCount; ++actionOffset) {
                        const uint16_t actionId =
                            graph.actionReferences[route.actionReferenceStart + actionOffset];
                        if (eventActionCount >= kGraphActionsPerEvent) {
                            return FlowEngineError::InvalidSection;
                        }
                        for (uint8_t existing = 0; existing < eventActionCount; ++existing) {
                            if (eventActions[existing] == actionId) {
                                return FlowEngineError::InvalidSection;
                            }
                        }
                        eventActions[eventActionCount++] = actionId;
                    }
                }
            }
        }
    }
    for (uint16_t index = 0; index < graph.referencedActionCount; ++index) {
        if (!referencedActionIds[graph.referencedActions[index].id]) {
            return FlowEngineError::InvalidSection;
        }
    }

    std::memcpy(inputIdByPin_, inputIdByPin, sizeof(inputIdByPin_));
    std::memcpy(routeSpans_, routeSpans, sizeof(routeSpans_));
    graph_ = &graph;
    resetRuntime(currentInputs, eventTokenSeed);
    return FlowEngineError::None;
}

void FlowEngine::deactivate() {
    graph_ = nullptr;
    std::memset(inputIdByPin_, 0, sizeof(inputIdByPin_));
    std::memset(routeSpans_, 0, sizeof(routeSpans_));
    resetRuntime(0, 1);
}

void FlowEngine::resetRuntime(uint16_t currentInputs, uint32_t eventTokenSeed) {
    currentInputs_ = currentInputs;
    nextEventToken_ = eventTokenSeed;
    nextDelayedSequence_ = 0;
    pendingEventHead_ = 0;
    pendingEventCount_ = 0;
    delayedInvocationCount_ = 0;
    for (DelayedInvocation &invocation : delayedInvocations_) {
        invocation.occupied = false;
    }
    actionQueueHead_ = 0;
    actionQueueCount_ = 0;
    counters_ = FlowEngineCounters{};
}

uint32_t FlowEngine::allocateEventToken() {
    const uint32_t token = nextEventToken_++;
    if (nextEventToken_ == 0) {
        nextEventToken_ = 1;
    }
    return token;
}

bool FlowEngine::submitInput(const FlowInputUpdate &update) {
    if (graph_ == nullptr) {
        increment(counters_.invalidInputUpdates);
        return false;
    }
    const uint16_t expectedChanged = static_cast<uint16_t>(currentInputs_ ^ update.current);
    const uint16_t expectedRising = static_cast<uint16_t>(expectedChanged & update.current);
    const uint16_t expectedFalling = static_cast<uint16_t>(expectedChanged & ~update.current);
    if (update.changed != expectedChanged || update.rising != expectedRising ||
        update.falling != expectedFalling || (update.rising & update.falling) != 0) {
        increment(counters_.invalidInputUpdates);
        return false;
    }

    bool accepted = true;
    for (uint8_t pin = 0; pin < kPinsPerDevice; ++pin) {
        const uint16_t mask = static_cast<uint16_t>(1u << pin);
        if ((update.changed & mask) == 0 || inputIdByPin_[pin] == 0) {
            continue;
        }
        const GraphEdge edge = (update.rising & mask) != 0 ? GraphEdge::Rising : GraphEdge::Falling;
        const RouteSpan &specific = routeSpans_[pin][edgeIndex(edge)];
        const RouteSpan &changed = routeSpans_[pin][edgeIndex(GraphEdge::Changed)];
        if (specific.count == 0 && changed.count == 0) {
            continue;
        }

        const uint32_t token = allocateEventToken();
        if (pendingEventCount_ >= kFlowPendingEventCapacity) {
            increment(counters_.pendingEventOverflows);
            accepted = false;
            continue;
        }
        const std::size_t tail =
            (pendingEventHead_ + pendingEventCount_) % kFlowPendingEventCapacity;
        PendingEvent &event = pendingEvents_[tail];
        event.inputPin = pin;
        event.token = token;
        event.occurredAt = update.nowMilliseconds;
        event.transitionEdge = edge;
        event.sourceValue = (update.current & mask) != 0;
        event.phase = 0;
        event.routeOffset = 0;
        event.actionOffset = 0;
        ++pendingEventCount_;
        increment(counters_.acceptedEvents);
    }
    currentInputs_ = update.current;
    return accepted;
}

bool FlowEngine::nextPendingAction(FlowActionInvocation &invocation, uint32_t &occurredAt,
                                   const GraphActionRecord *&action) {
    while (pendingEventCount_ != 0) {
        PendingEvent &event = pendingEvents_[pendingEventHead_];
        if (event.phase >= 2) {
            pendingEventHead_ = (pendingEventHead_ + 1) % kFlowPendingEventCapacity;
            --pendingEventCount_;
            continue;
        }
        const GraphEdge selector = event.phase == 0 ? event.transitionEdge : GraphEdge::Changed;
        const RouteSpan &span = routeSpans_[event.inputPin][edgeIndex(selector)];
        if (event.routeOffset >= span.count) {
            ++event.phase;
            event.routeOffset = 0;
            event.actionOffset = 0;
            continue;
        }

        const LocalGraphRoute &route = graph_->routes[span.start + event.routeOffset];
        if (event.actionOffset >= route.actionCount) {
            ++event.routeOffset;
            event.actionOffset = 0;
            continue;
        }
        const uint16_t actionId =
            graph_->actionReferences[route.actionReferenceStart + event.actionOffset];
        ++event.actionOffset;
        action = graph_->findAction(actionId);
        if (action == nullptr) {
            return false;
        }
        invocation.generation = graph_->identity.generation;
        invocation.eventToken = event.token;
        invocation.actionId = actionId;
        invocation.sourceDevice = device_;
        invocation.targetDevice = action->targetDevice;
        invocation.sourceValue = event.sourceValue;
        occurredAt = event.occurredAt;
        if (event.actionOffset >= route.actionCount) {
            event.actionOffset = 0;
            ++event.routeOffset;
        }
        if (event.routeOffset >= span.count) {
            event.routeOffset = 0;
            ++event.phase;
            while (event.phase < 2) {
                const GraphEdge nextSelector =
                    event.phase == 0 ? event.transitionEdge : GraphEdge::Changed;
                if (routeSpans_[event.inputPin][edgeIndex(nextSelector)].count != 0) {
                    break;
                }
                ++event.phase;
            }
        }
        if (event.phase >= 2) {
            pendingEventHead_ = (pendingEventHead_ + 1) % kFlowPendingEventCapacity;
            --pendingEventCount_;
        }
        return true;
    }
    return false;
}

std::size_t FlowEngine::nextDueInvocation(uint32_t nowMilliseconds) const {
    std::size_t selected = kFlowDelayedInvocationCapacity;
    uint32_t selectedLateness = 0;
    uint32_t selectedSequence = 0;
    for (std::size_t index = 0; index < kFlowDelayedInvocationCapacity; ++index) {
        const DelayedInvocation &candidate = delayedInvocations_[index];
        if (!candidate.occupied || !timeReached(nowMilliseconds, candidate.dueAt)) {
            continue;
        }
        const uint32_t lateness = nowMilliseconds - candidate.dueAt;
        if (selected == kFlowDelayedInvocationCapacity || lateness > selectedLateness ||
            (lateness == selectedLateness &&
             static_cast<int32_t>(candidate.sequence - selectedSequence) < 0)) {
            selected = index;
            selectedLateness = lateness;
            selectedSequence = candidate.sequence;
        }
    }
    return selected;
}

bool FlowEngine::queueAction(const FlowActionInvocation &invocation) {
    if (actionQueueCount_ >= kFlowActionQueueCapacity) {
        return false;
    }
    const std::size_t tail = (actionQueueHead_ + actionQueueCount_) % kFlowActionQueueCapacity;
    actionQueue_[tail] = invocation;
    ++actionQueueCount_;
    return true;
}

std::size_t FlowEngine::service(uint32_t nowMilliseconds) {
    if (graph_ == nullptr) {
        return 0;
    }

    std::size_t work = 0;
    while (work < kFlowServiceBudget) {
        const std::size_t due = nextDueInvocation(nowMilliseconds);
        if (due != kFlowDelayedInvocationCapacity) {
            if (actionQueueCount_ >= kFlowActionQueueCapacity) {
                increment(counters_.actionQueueBackpressure);
                break;
            }
            queueAction(delayedInvocations_[due].invocation);
            delayedInvocations_[due].occupied = false;
            --delayedInvocationCount_;
            ++work;
            continue;
        }
        if (pendingEventCount_ == 0) {
            break;
        }
        if (actionQueueCount_ >= kFlowActionQueueCapacity) {
            increment(counters_.actionQueueBackpressure);
            break;
        }

        FlowActionInvocation invocation{};
        uint32_t occurredAt = 0;
        const GraphActionRecord *action = nullptr;
        if (!nextPendingAction(invocation, occurredAt, action)) {
            break;
        }
        increment(counters_.evaluatedActions);
        if (action->delayMs == 0) {
            queueAction(invocation);
        } else if (delayedInvocationCount_ >= kFlowDelayedInvocationCapacity) {
            increment(counters_.delayedInvocationOverflows);
        } else {
            for (DelayedInvocation &delayed : delayedInvocations_) {
                if (delayed.occupied) {
                    continue;
                }
                delayed.invocation = invocation;
                delayed.dueAt = occurredAt + action->delayMs;
                delayed.sequence = nextDelayedSequence_++;
                delayed.occupied = true;
                ++delayedInvocationCount_;
                break;
            }
        }
        ++work;
    }
    return work;
}

bool FlowEngine::tryPopAction(FlowActionInvocation &invocation) {
    if (actionQueueCount_ == 0) {
        return false;
    }
    invocation = actionQueue_[actionQueueHead_];
    actionQueueHead_ = (actionQueueHead_ + 1) % kFlowActionQueueCapacity;
    --actionQueueCount_;
    return true;
}

}  // namespace piho
