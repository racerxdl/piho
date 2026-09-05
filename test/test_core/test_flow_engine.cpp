#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "piho/flow_engine.h"
#include "piho/graph_image.h"

namespace {

constexpr const char *kGoldenPath = "tools/piho-flow/test/fixtures/synthetic.phg";

std::vector<uint8_t> loadGoldenImage() {
    std::ifstream input(kGoldenPath, std::ios::binary);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "cannot open shared synthetic.phg fixture");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

piho::LocalInputGraph emptyGraph(uint8_t device = 1) {
    piho::LocalInputGraph graph{};
    graph.identity.format = piho::kGraphFormatVersion;
    graph.identity.executorApi = piho::kGraphExecutorApiVersion;
    graph.identity.generation = 7;
    graph.identity.checksum = 0x12345678;
    graph.device = device;
    graph.role = piho::GraphDeviceRole::Input;
    graph.inputCount = 1;
    graph.inputs[0] = piho::GraphInputRecord{1, device, 0, 25};
    return graph;
}

void addSetAction(piho::LocalInputGraph &graph, uint16_t id, uint32_t delayMs = 0) {
    const uint16_t slot = graph.referencedActionCount++;
    piho::GraphActionRecord &action = graph.referencedActions[slot];
    action.id = id;
    action.targetDevice = 7;
    action.targetPin = static_cast<uint8_t>((id - 1) % piho::kPinsPerDevice);
    action.operation = piho::GraphOperation::Set;
    action.value = true;
    action.delayMs = delayMs;
    graph.actionSlotById[id] = static_cast<uint16_t>(slot + 1);
}

void addRoute(piho::LocalInputGraph &graph, uint16_t id, piho::GraphEdge edge,
              uint16_t firstActionId, uint8_t actionCount) {
    piho::LocalGraphRoute &route = graph.routes[graph.routeCount++];
    route.id = id;
    route.flowId = id;
    route.inputId = 1;
    route.actionReferenceStart = graph.actionReferenceCount;
    route.edge = edge;
    route.actionCount = actionCount;
    for (uint8_t offset = 0; offset < actionCount; ++offset) {
        graph.actionReferences[graph.actionReferenceCount++] =
            static_cast<uint16_t>(firstActionId + offset);
    }
}

piho::FlowInputUpdate transition(uint16_t previous, uint16_t current, uint32_t nowMilliseconds) {
    const uint16_t changed = static_cast<uint16_t>(previous ^ current);
    return piho::FlowInputUpdate{current, changed, static_cast<uint16_t>(changed & current),
                                 static_cast<uint16_t>(changed & ~current), nowMilliseconds};
}

void test_flow_engine_runs_shared_graph_without_synthetic_activation_edge() {
    const std::vector<uint8_t> image = loadGoldenImage();
    const piho::GraphImageSource source =
        piho::GraphImageSource::fromMemory(image.data(), image.size());
    piho::GraphManifest manifest{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::None),
                            static_cast<uint8_t>(piho::GraphImageCodec::validate(source, manifest)));

    piho::LocalInputGraph deviceOne{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::loadInputSection(source, manifest, 1, deviceOne)));
    piho::FlowEngine engine(1);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::FlowEngineError::None),
        static_cast<uint8_t>(engine.activate(deviceOne, static_cast<uint16_t>(1u << 2), 900)));
    TEST_ASSERT_EQUAL_UINT32(0, engine.service(UINT32_MAX - 101));
    piho::FlowActionInvocation invocation{};
    TEST_ASSERT_FALSE(engine.tryPopAction(invocation));

    TEST_ASSERT_TRUE(engine.submitInput(
        transition(static_cast<uint16_t>(1u << 2), 0, UINT32_MAX - 100)));
    TEST_ASSERT_EQUAL_UINT32(1, engine.service(UINT32_MAX - 100));
    TEST_ASSERT_EQUAL_UINT32(1, engine.delayedInvocationCount());
    TEST_ASSERT_EQUAL_UINT32(0, engine.service(148));
    TEST_ASSERT_EQUAL_UINT32(1, engine.service(149));
    TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
    TEST_ASSERT_EQUAL_UINT16(3, invocation.actionId);
    TEST_ASSERT_EQUAL_UINT32(900, invocation.eventToken);
    TEST_ASSERT_EQUAL_UINT8(1, invocation.sourceDevice);
    TEST_ASSERT_EQUAL_UINT8(7, invocation.targetDevice);
    TEST_ASSERT_FALSE(invocation.sourceValue);

    TEST_ASSERT_TRUE(engine.submitInput(transition(0, static_cast<uint16_t>(1u << 2), 400)));
    TEST_ASSERT_EQUAL_UINT32(2, engine.service(400));
    TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
    TEST_ASSERT_EQUAL_UINT16(4, invocation.actionId);
    TEST_ASSERT_EQUAL_UINT32(901, invocation.eventToken);
    TEST_ASSERT_TRUE(invocation.sourceValue);
    TEST_ASSERT_EQUAL_UINT32(0, engine.service(60399));
    TEST_ASSERT_EQUAL_UINT32(1, engine.service(60400));
    TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
    TEST_ASSERT_EQUAL_UINT16(1, invocation.actionId);
    TEST_ASSERT_EQUAL_UINT32(901, invocation.eventToken);
    TEST_ASSERT_FALSE(engine.tryPopAction(invocation));

    piho::LocalInputGraph deviceTwo{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::loadInputSection(source, manifest, 2, deviceTwo)));
    piho::FlowEngine secondEngine(2);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(secondEngine.activate(deviceTwo, 0, 1200)));
    TEST_ASSERT_TRUE(secondEngine.submitInput(transition(0, static_cast<uint16_t>(1u << 5), 10)));
    TEST_ASSERT_EQUAL_UINT32(1, secondEngine.service(10));
    TEST_ASSERT_TRUE(secondEngine.tryPopAction(invocation));
    TEST_ASSERT_EQUAL_UINT16(2, invocation.actionId);
    TEST_ASSERT_EQUAL_UINT32(1200, invocation.eventToken);
    TEST_ASSERT_TRUE(invocation.sourceValue);
}

void test_flow_engine_combines_matching_routes_under_one_event_token() {
    piho::LocalInputGraph graph = emptyGraph();
    addSetAction(graph, 1);
    addSetAction(graph, 2);
    addRoute(graph, 1, piho::GraphEdge::Rising, 1, 1);
    addRoute(graph, 2, piho::GraphEdge::Changed, 2, 1);

    piho::FlowEngine engine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(engine.activate(graph, 0, 44)));
    TEST_ASSERT_TRUE(engine.submitInput(transition(0, 1, 10)));
    TEST_ASSERT_EQUAL_UINT32(2, engine.service(10));

    piho::FlowActionInvocation first{};
    piho::FlowActionInvocation second{};
    TEST_ASSERT_TRUE(engine.tryPopAction(first));
    TEST_ASSERT_TRUE(engine.tryPopAction(second));
    TEST_ASSERT_EQUAL_UINT16(1, first.actionId);
    TEST_ASSERT_EQUAL_UINT16(2, second.actionId);
    TEST_ASSERT_EQUAL_UINT32(first.eventToken, second.eventToken);
    TEST_ASSERT_EQUAL_UINT32(44, first.eventToken);
    TEST_ASSERT_EQUAL_UINT32(1, engine.counters().acceptedEvents);
    TEST_ASSERT_EQUAL_UINT32(2, engine.counters().evaluatedActions);
}

void test_flow_engine_limits_each_service_call_without_losing_fanout() {
    piho::LocalInputGraph graph = emptyGraph();
    for (uint16_t id = 1; id <= piho::kGraphActionsPerEvent; ++id) {
        addSetAction(graph, id);
    }
    addRoute(graph, 1, piho::GraphEdge::Rising, 1, piho::kGraphActionsPerEvent);

    piho::FlowEngine engine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(engine.activate(graph, 0, 1)));
    TEST_ASSERT_TRUE(engine.submitInput(transition(0, 1, 0)));
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowServiceBudget, engine.service(0));
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowServiceBudget, engine.queuedActionCount());

    piho::FlowActionInvocation invocation{};
    for (uint16_t id = 1; id <= piho::kFlowServiceBudget; ++id) {
        TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
        TEST_ASSERT_EQUAL_UINT16(id, invocation.actionId);
    }
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowServiceBudget, engine.service(0));
    for (uint16_t id = piho::kFlowServiceBudget + 1; id <= piho::kGraphActionsPerEvent; ++id) {
        TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
        TEST_ASSERT_EQUAL_UINT16(id, invocation.actionId);
    }
    TEST_ASSERT_EQUAL_UINT32(0, engine.pendingEventCount());
    TEST_ASSERT_EQUAL_UINT32(piho::kGraphActionsPerEvent, engine.counters().evaluatedActions);
}

void test_flow_engine_reports_pending_and_delayed_capacity_exhaustion() {
    piho::LocalInputGraph immediate = emptyGraph();
    addSetAction(immediate, 1);
    addRoute(immediate, 1, piho::GraphEdge::Rising, 1, 1);
    addRoute(immediate, 2, piho::GraphEdge::Falling, 1, 1);

    piho::FlowEngine pendingEngine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(pendingEngine.activate(immediate, 0, 1)));
    uint16_t previous = 0;
    for (std::size_t index = 0; index < piho::kFlowPendingEventCapacity; ++index) {
        const uint16_t current = previous == 0 ? 1 : 0;
        TEST_ASSERT_TRUE(pendingEngine.submitInput(transition(previous, current, index)));
        previous = current;
    }
    const uint16_t overflowState = previous == 0 ? 1 : 0;
    TEST_ASSERT_FALSE(pendingEngine.submitInput(
        transition(previous, overflowState, piho::kFlowPendingEventCapacity)));
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowPendingEventCapacity, pendingEngine.pendingEventCount());
    TEST_ASSERT_EQUAL_UINT32(1, pendingEngine.counters().pendingEventOverflows);

    piho::LocalInputGraph delayed = emptyGraph();
    addSetAction(delayed, 1, 100000);
    addRoute(delayed, 1, piho::GraphEdge::Rising, 1, 1);
    addRoute(delayed, 2, piho::GraphEdge::Falling, 1, 1);
    piho::FlowEngine delayedEngine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(delayedEngine.activate(delayed, 0, 100)));
    previous = 0;
    for (std::size_t index = 0; index <= piho::kFlowDelayedInvocationCapacity; ++index) {
        const uint16_t current = previous == 0 ? 1 : 0;
        TEST_ASSERT_TRUE(delayedEngine.submitInput(transition(previous, current, index)));
        TEST_ASSERT_EQUAL_UINT32(1, delayedEngine.service(index));
        previous = current;
    }
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowDelayedInvocationCapacity,
                             delayedEngine.delayedInvocationCount());
    TEST_ASSERT_EQUAL_UINT32(1, delayedEngine.counters().delayedInvocationOverflows);
}

void test_flow_engine_backpressures_without_dropping_an_accepted_action() {
    piho::LocalInputGraph graph = emptyGraph();
    addSetAction(graph, 1);
    addRoute(graph, 1, piho::GraphEdge::Rising, 1, 1);
    addRoute(graph, 2, piho::GraphEdge::Falling, 1, 1);
    piho::FlowEngine engine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(engine.activate(graph, 0, 1)));

    uint16_t previous = 0;
    for (std::size_t index = 0; index < piho::kFlowActionQueueCapacity; ++index) {
        const uint16_t current = previous == 0 ? 1 : 0;
        TEST_ASSERT_TRUE(engine.submitInput(transition(previous, current, index)));
        TEST_ASSERT_EQUAL_UINT32(1, engine.service(index));
        previous = current;
    }
    TEST_ASSERT_EQUAL_UINT32(0, engine.counters().actionQueueBackpressure);

    const uint16_t next = previous == 0 ? 1 : 0;
    TEST_ASSERT_TRUE(engine.submitInput(transition(previous, next, 40)));
    TEST_ASSERT_EQUAL_UINT32(0, engine.service(40));
    TEST_ASSERT_EQUAL_UINT32(1, engine.counters().actionQueueBackpressure);
    TEST_ASSERT_EQUAL_UINT32(1, engine.pendingEventCount());

    piho::FlowActionInvocation invocation{};
    TEST_ASSERT_TRUE(engine.tryPopAction(invocation));
    TEST_ASSERT_EQUAL_UINT32(1, engine.service(40));
    TEST_ASSERT_EQUAL_UINT32(piho::kFlowActionQueueCapacity, engine.queuedActionCount());
    TEST_ASSERT_EQUAL_UINT32(33, engine.counters().evaluatedActions);
}

void test_flow_engine_rejects_mismatched_activation_and_input_masks() {
    piho::LocalInputGraph graph = emptyGraph();
    addSetAction(graph, 1);
    addRoute(graph, 1, piho::GraphEdge::Rising, 1, 1);
    piho::FlowEngine engine(1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::None),
                            static_cast<uint8_t>(engine.activate(graph, 0, 77)));

    piho::LocalInputGraph incompatible = graph;
    incompatible.identity.executorApi = 2;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::IncompatibleGraph),
                            static_cast<uint8_t>(engine.activate(incompatible, 1, 88)));
    TEST_ASSERT_TRUE(engine.active());
    TEST_ASSERT_EQUAL_HEX16(0, engine.currentInputs());

    incompatible = graph;
    incompatible.role = piho::GraphDeviceRole::Output;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::RoleMismatch),
                            static_cast<uint8_t>(engine.activate(incompatible, 1, 88)));
    incompatible = graph;
    incompatible.device = 2;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FlowEngineError::InvalidDevice),
                            static_cast<uint8_t>(engine.activate(incompatible, 1, 88)));

    piho::FlowInputUpdate invalid = transition(0, 1, 10);
    invalid.falling = 1;
    TEST_ASSERT_FALSE(engine.submitInput(invalid));
    TEST_ASSERT_EQUAL_HEX16(0, engine.currentInputs());
    TEST_ASSERT_EQUAL_UINT32(1, engine.counters().invalidInputUpdates);
    TEST_ASSERT_EQUAL_UINT32(0, engine.pendingEventCount());
}

}  // namespace

void runFlowEngineTests() {
    RUN_TEST(test_flow_engine_runs_shared_graph_without_synthetic_activation_edge);
    RUN_TEST(test_flow_engine_combines_matching_routes_under_one_event_token);
    RUN_TEST(test_flow_engine_limits_each_service_call_without_losing_fanout);
    RUN_TEST(test_flow_engine_reports_pending_and_delayed_capacity_exhaustion);
    RUN_TEST(test_flow_engine_backpressures_without_dropping_an_accepted_action);
    RUN_TEST(test_flow_engine_rejects_mismatched_activation_and_input_masks);
}
