#include "piho/runtime.h"

namespace piho {
namespace {

bool sameIdentity(const GraphIdentity &left, const GraphIdentity &right) {
    return left.generation == right.generation &&
           left.checksum == right.checksum;
}

}  // namespace

bool InputGraphRuntime::activate(uint16_t currentInputs) {
    const GraphStoreStatus &stored = store_.status();
    if (!store_.hasActiveGraph()) {
        return false;
    }
    if (engine_.active() &&
        sameIdentity(graphs_[activeGraphIndex_].identity, stored.active)) {
        return true;
    }

    const uint8_t stagingIndex =
        engine_.active() ? static_cast<uint8_t>(activeGraphIndex_ ^ 1u) : 0;
    LocalInputGraph &staging = graphs_[stagingIndex];
    if (store_.loadActiveInput(device_, staging) != GraphStoreError::None ||
        engine_.activate(staging, currentInputs, 1) != FlowEngineError::None) {
        return false;
    }
    sender_.clear();
    activeGraphIndex_ = stagingIndex;
    return true;
}

void InputGraphRuntime::deactivate() {
    engine_.deactivate();
    sender_.clear();
}

bool InputGraphRuntime::submitInput(const FlowInputUpdate &update) {
    return engine_.submitInput(update);
}

std::size_t InputGraphRuntime::service(uint32_t nowMilliseconds) {
    std::size_t work = engine_.service(nowMilliseconds);
    while (sender_.pendingCount() < kActionPendingCapacity) {
        FlowActionInvocation invocation{};
        if (!engine_.tryPopAction(invocation)) {
            break;
        }
        sender_.enqueue(invocation, nowMilliseconds);
    }
    work += sender_.service(nowMilliseconds);
    return work;
}

bool InputGraphRuntime::acknowledge(
    const ActionAcknowledgement &acknowledgement, uint32_t nowMilliseconds) {
    return sender_.acknowledge(acknowledgement, nowMilliseconds);
}

const LocalInputGraph *InputGraphRuntime::activeGraph() const {
    return engine_.active() ? &graphs_[activeGraphIndex_] : nullptr;
}

GraphRuntimeStatus InputGraphRuntime::status() const {
    GraphRuntimeStatus result{};
    if (engine_.active()) {
        result.identity = graphs_[activeGraphIndex_].identity;
    }
    const FlowEngineCounters &flow = engine_.counters();
    const ReliableActionCounters &actions = sender_.counters();
    result.flowAcceptedEvents = flow.acceptedEvents;
    result.flowEvaluatedActions = flow.evaluatedActions;
    result.actionRetries = actions.retries;
    result.actionRejections = actions.rejections;
    return result;
}

bool OutputGraphRuntime::activate(uint16_t currentOutputs) {
    const GraphStoreStatus &stored = store_.status();
    if (!store_.hasActiveGraph()) {
        return false;
    }
    if (executor_.active() &&
        sameIdentity(graphs_[activeGraphIndex_].identity, stored.active)) {
        return true;
    }

    const uint8_t stagingIndex =
        executor_.active() ? static_cast<uint8_t>(activeGraphIndex_ ^ 1u) : 0;
    LocalOutputGraph &staging = graphs_[stagingIndex];
    if (store_.loadActiveOutput(device_, staging) != GraphStoreError::None ||
        executor_.activate(staging, currentOutputs) !=
            OutputActionActivationError::None) {
        return false;
    }
    activeGraphIndex_ = stagingIndex;
    return true;
}

void OutputGraphRuntime::deactivate() { executor_.deactivate(); }

ActionAcknowledgement OutputGraphRuntime::execute(const ActionRequest &request,
                                                  uint32_t nowMilliseconds) {
    return executor_.execute(request, nowMilliseconds);
}

std::size_t OutputGraphRuntime::service(uint32_t nowMilliseconds) {
    return executor_.service(nowMilliseconds);
}

void OutputGraphRuntime::synchronizeOutputs(uint16_t currentOutputs) {
    executor_.synchronizeOutputs(currentOutputs);
}

const LocalOutputGraph *OutputGraphRuntime::activeGraph() const {
    return executor_.active() ? &graphs_[activeGraphIndex_] : nullptr;
}

GraphRuntimeStatus OutputGraphRuntime::status() const {
    GraphRuntimeStatus result{};
    if (executor_.active()) {
        result.identity = graphs_[activeGraphIndex_].identity;
    }
    const OutputActionCounters &executor = executor_.counters();
    result.executorExecutedActions = executor.executed;
    result.executorRejectedActions = executor.rejected;
    return result;
}

}  // namespace piho
