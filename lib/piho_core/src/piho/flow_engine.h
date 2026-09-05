#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/graph_image.h"
#include "piho/protocol.h"

namespace piho {

constexpr std::size_t kFlowPendingEventCapacity = 32;
constexpr std::size_t kFlowDelayedInvocationCapacity = 32;
constexpr std::size_t kFlowActionQueueCapacity = 32;
constexpr std::size_t kFlowServiceBudget = 8;

enum class FlowEngineError : uint8_t {
    None,
    InvalidDevice,
    RoleMismatch,
    IncompatibleGraph,
    InvalidSection,
};

struct FlowInputUpdate {
    uint16_t current = 0;
    uint16_t changed = 0;
    uint16_t rising = 0;
    uint16_t falling = 0;
    uint32_t nowMilliseconds = 0;
};

struct FlowActionInvocation {
    uint32_t generation = 0;
    uint32_t eventToken = 0;
    uint16_t actionId = 0;
    uint8_t sourceDevice = 0;
    uint8_t targetDevice = 0;
    bool sourceValue = false;
};

struct FlowEngineCounters {
    uint32_t acceptedEvents = 0;
    uint32_t evaluatedActions = 0;
    uint32_t invalidInputUpdates = 0;
    uint32_t pendingEventOverflows = 0;
    uint32_t delayedInvocationOverflows = 0;
    uint32_t actionQueueBackpressure = 0;
};

class FlowEngine {
   public:
    explicit FlowEngine(uint8_t device) : device_(device) {}

    // The active graph must remain immutable and alive until deactivation or replacement.
    // eventTokenSeed must be in 1..kActionEventTokenMaximum. Activation is transactional:
    // failure leaves the previous graph and runtime state intact.
    FlowEngineError activate(const LocalInputGraph &graph, uint16_t currentInputs,
                             uint32_t eventTokenSeed);
    void deactivate();

    // Masks must describe the exact transition from currentInputs() to update.current.
    bool submitInput(const FlowInputUpdate &update);
    // Services at most kFlowServiceBudget action evaluations or due emissions.
    std::size_t service(uint32_t nowMilliseconds);
    bool tryPopAction(FlowActionInvocation &invocation);

    bool active() const { return graph_ != nullptr; }
    uint16_t currentInputs() const { return currentInputs_; }
    std::size_t pendingEventCount() const { return pendingEventCount_; }
    std::size_t delayedInvocationCount() const { return delayedInvocationCount_; }
    std::size_t queuedActionCount() const { return actionQueueCount_; }
    const FlowEngineCounters &counters() const { return counters_; }

   private:
    struct RouteSpan {
        uint16_t start = 0;
        uint16_t count = 0;
    };

    struct PendingEvent {
        uint8_t inputPin = 0;
        uint32_t token = 0;
        uint32_t occurredAt = 0;
        GraphEdge transitionEdge = GraphEdge::Rising;
        bool sourceValue = false;
        uint8_t phase = 0;
        uint16_t routeOffset = 0;
        uint8_t actionOffset = 0;
    };

    struct DelayedInvocation {
        FlowActionInvocation invocation{};
        uint32_t dueAt = 0;
        uint32_t sequence = 0;
        bool occupied = false;
    };

    static std::size_t edgeIndex(GraphEdge edge);
    static bool timeReached(uint32_t now, uint32_t deadline);
    static void increment(uint32_t &counter);

    void resetRuntime(uint16_t currentInputs, uint32_t eventTokenSeed);
    uint32_t allocateEventToken();
    bool nextPendingAction(FlowActionInvocation &invocation, uint32_t &occurredAt,
                           const GraphActionRecord *&action);
    std::size_t nextDueInvocation(uint32_t nowMilliseconds) const;
    bool queueAction(const FlowActionInvocation &invocation);

    uint8_t device_ = 0;
    const LocalInputGraph *graph_ = nullptr;
    uint16_t currentInputs_ = 0;
    uint32_t nextEventToken_ = 1;
    uint32_t nextDelayedSequence_ = 0;
    uint16_t inputIdByPin_[kPinsPerDevice]{};
    RouteSpan routeSpans_[kPinsPerDevice][3]{};

    PendingEvent pendingEvents_[kFlowPendingEventCapacity]{};
    std::size_t pendingEventHead_ = 0;
    std::size_t pendingEventCount_ = 0;
    DelayedInvocation delayedInvocations_[kFlowDelayedInvocationCapacity]{};
    std::size_t delayedInvocationCount_ = 0;
    FlowActionInvocation actionQueue_[kFlowActionQueueCapacity]{};
    std::size_t actionQueueHead_ = 0;
    std::size_t actionQueueCount_ = 0;
    FlowEngineCounters counters_{};
};

}  // namespace piho
