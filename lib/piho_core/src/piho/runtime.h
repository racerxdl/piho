#pragma once

#include <cstddef>
#include <cstdint>

#include "piho/action_runtime.h"
#include "piho/graph_store.h"

namespace piho {

struct GraphRuntimeStatus {
    GraphIdentity identity{};
    uint32_t flowAcceptedEvents = 0;
    uint32_t flowEvaluatedActions = 0;
    uint32_t actionRetries = 0;
    uint32_t actionRejections = 0;
    uint32_t executorExecutedActions = 0;
    uint32_t executorRejectedActions = 0;
};

class InputGraphRuntime {
   public:
    InputGraphRuntime(uint8_t device, GraphStore &store,
                      ActionFrameWrite writeFrame, void *writeContext = nullptr)
        : device_(device), store_(store), engine_(device),
          sender_(device, writeFrame, writeContext) {}

    // Loads into an inactive buffer and swaps only after complete validation.
    bool activate(uint16_t currentInputs);
    void deactivate();

    bool submitInput(const FlowInputUpdate &update);
    std::size_t service(uint32_t nowMilliseconds);
    bool acknowledge(const ActionAcknowledgement &acknowledgement,
                     uint32_t nowMilliseconds);

    bool active() const { return engine_.active(); }
    const LocalInputGraph *activeGraph() const;
    GraphRuntimeStatus status() const;

   private:
    uint8_t device_ = 0;
    GraphStore &store_;
    FlowEngine engine_;
    ReliableActionSender sender_;
    LocalInputGraph graphs_[2]{};
    uint8_t activeGraphIndex_ = 0;
};

class OutputGraphRuntime {
   public:
    OutputGraphRuntime(uint8_t device, GraphStore &store,
                       OutputPinWrite writePin, void *writeContext = nullptr)
        : device_(device), store_(store),
          executor_(device, writePin, writeContext) {}

    // Loads into an inactive buffer and swaps only after complete validation.
    bool activate(uint16_t currentOutputs);
    void deactivate();

    ActionAcknowledgement execute(const ActionRequest &request,
                                  uint32_t nowMilliseconds);
    std::size_t service(uint32_t nowMilliseconds);
    void synchronizeOutputs(uint16_t currentOutputs);

    bool active() const { return executor_.active(); }
    const LocalOutputGraph *activeGraph() const;
    GraphRuntimeStatus status() const;

   private:
    uint8_t device_ = 0;
    GraphStore &store_;
    OutputActionExecutor executor_;
    LocalOutputGraph graphs_[2]{};
    uint8_t activeGraphIndex_ = 0;
};

}  // namespace piho
