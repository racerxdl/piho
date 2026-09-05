#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/flow_engine.h"
#include "piho/graph_image.h"
#include "piho/protocol.h"

namespace piho {

constexpr std::size_t kActionPendingCapacity = 32;
constexpr std::size_t kActionDeduplicationCapacity = 64;
constexpr std::size_t kActionRuntimeServiceBudget = 8;
constexpr uint32_t kActionAcknowledgementTimeoutMs = 100;
constexpr uint8_t kActionMaximumAttempts = 3;
constexpr uint32_t kActionRetryLifetimeMs =
    kActionAcknowledgementTimeoutMs * kActionMaximumAttempts;
constexpr uint32_t kActionDeduplicationWindowMs = 1000;

static_assert(kActionDeduplicationWindowMs > kActionRetryLifetimeMs);

enum class OutputActionActivationError : uint8_t {
    None,
    InvalidDevice,
    RoleMismatch,
    IncompatibleGraph,
    InvalidSection,
};

using OutputPinWrite = bool (*)(void *context, uint8_t localPin, bool value);

struct OutputActionCounters {
    uint32_t executed = 0;
    uint32_t duplicates = 0;
    uint32_t wrongGenerations = 0;
    uint32_t unknownActions = 0;
    uint32_t wrongTargets = 0;
    uint32_t invalidActions = 0;
    uint32_t unavailableOutputs = 0;
    uint32_t deduplicationOverflows = 0;
    uint32_t completedPulses = 0;
    uint32_t pulseCompletionFailures = 0;
};

class OutputActionExecutor {
   public:
    OutputActionExecutor(uint8_t device, OutputPinWrite writePin, void *writeContext = nullptr)
        : device_(device), writePin_(writePin), writeContext_(writeContext) {}

    // The active graph must remain immutable and alive until deactivation or replacement.
    OutputActionActivationError activate(const LocalOutputGraph &graph, uint16_t currentOutputs);
    void deactivate();

    ActionAcknowledgement execute(const ActionRequest &request, uint32_t nowMilliseconds);
    std::size_t service(uint32_t nowMilliseconds);

    bool active() const { return graph_ != nullptr; }
    uint16_t currentOutputs() const { return currentOutputs_; }
    std::size_t activePulseCount() const { return activePulseCount_; }
    const OutputActionCounters &counters() const { return counters_; }

   private:
    struct DeduplicationEntry {
        uint32_t eventToken = 0;
        uint16_t actionId = 0;
        uint8_t sourceDevice = 0;
        uint32_t expiresAt = 0;
        bool occupied = false;
    };

    struct PulseTimer {
        uint32_t dueAt = 0;
        bool occupied = false;
    };

    static bool timeReached(uint32_t now, uint32_t deadline);
    static void increment(uint32_t &counter);

    void resetRuntime(uint16_t currentOutputs);
    ActionAcknowledgement acknowledgement(const ActionRequest &request,
                                            ActionAckStatus status) const;
    bool writeLogicalPin(uint8_t pin, bool value);

    uint8_t device_ = 0;
    OutputPinWrite writePin_ = nullptr;
    void *writeContext_ = nullptr;
    const LocalOutputGraph *graph_ = nullptr;
    uint16_t currentOutputs_ = 0;
    DeduplicationEntry deduplication_[kActionDeduplicationCapacity]{};
    PulseTimer pulseTimers_[kPinsPerDevice]{};
    std::size_t activePulseCount_ = 0;
    OutputActionCounters counters_{};
};

using ActionFrameWrite = bool (*)(void *context, const CanFrame &frame);

struct ReliableActionCounters {
    uint32_t enqueued = 0;
    uint32_t transmissionAttempts = 0;
    uint32_t retries = 0;
    uint32_t sendFailures = 0;
    uint32_t acknowledged = 0;
    uint32_t alreadyExecutedAcknowledgements = 0;
    uint32_t rejections = 0;
    uint32_t unavailableAcknowledgements = 0;
    uint32_t exhausted = 0;
    uint32_t timeouts = 0;
    uint32_t queueOverflows = 0;
    uint32_t invalidEnqueues = 0;
    uint32_t duplicateEnqueues = 0;
    uint32_t unexpectedAcknowledgements = 0;
};

class ReliableActionSender {
   public:
    ReliableActionSender(uint8_t sourceDevice, ActionFrameWrite writeFrame,
                         void *writeContext = nullptr)
        : sourceDevice_(sourceDevice), writeFrame_(writeFrame), writeContext_(writeContext) {}

    bool enqueue(const FlowActionInvocation &invocation, uint32_t nowMilliseconds);
    std::size_t service(uint32_t nowMilliseconds);
    bool acknowledge(const ActionAcknowledgement &acknowledgement, uint32_t nowMilliseconds);
    void clear();

    std::size_t pendingCount() const { return pendingCount_; }
    const ReliableActionCounters &counters() const { return counters_; }

   private:
    struct PendingAction {
        ActionRequest request{};
        uint32_t retryAt = 0;
        uint32_t sequence = 0;
        uint32_t expiresAt = 0;
        uint8_t attempts = 0;
        bool awaitingAcknowledgement = false;
        bool occupied = false;
    };

    static bool timeReached(uint32_t now, uint32_t deadline);
    static void increment(uint32_t &counter);
    std::size_t nextDue(uint32_t nowMilliseconds) const;

    uint8_t sourceDevice_ = 0;
    ActionFrameWrite writeFrame_ = nullptr;
    void *writeContext_ = nullptr;
    PendingAction pending_[kActionPendingCapacity]{};
    std::size_t pendingCount_ = 0;
    uint32_t nextSequence_ = 0;
    ReliableActionCounters counters_{};
};

}  // namespace piho
