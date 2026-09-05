#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "piho/action_runtime.h"
#include "piho/protocol.h"

namespace {

constexpr uint32_t kGeneration = 42;
constexpr uint8_t kSourceDevice = 1;
constexpr uint8_t kOutputDevice = 7;

piho::LocalOutputGraph outputGraph() {
    piho::LocalOutputGraph graph{};
    graph.identity.format = piho::kGraphFormatVersion;
    graph.identity.executorApi = piho::kGraphExecutorApiVersion;
    graph.identity.generation = kGeneration;
    graph.identity.checksum = 0x12345678;
    graph.device = kOutputDevice;
    graph.role = piho::GraphDeviceRole::Output;
    graph.actionCount = 6;
    graph.actions[0] =
        piho::GraphActionRecord{1, kOutputDevice, 0, piho::GraphOperation::Set, true, 0, 0};
    graph.actions[1] = piho::GraphActionRecord{
        2, kOutputDevice, 1, piho::GraphOperation::CopySource, false, 0, 0};
    graph.actions[2] =
        piho::GraphActionRecord{3, kOutputDevice, 2, piho::GraphOperation::Toggle, false, 0, 0};
    graph.actions[3] =
        piho::GraphActionRecord{4, kOutputDevice, 3, piho::GraphOperation::Pulse, false, 0, 100};
    graph.actions[4] =
        piho::GraphActionRecord{5, kOutputDevice, 0, piho::GraphOperation::Set, false, 0, 0};
    graph.actions[5] =
        piho::GraphActionRecord{6, kOutputDevice, 3, piho::GraphOperation::Set, true, 0, 0};
    for (uint16_t index = 0; index < graph.actionCount; ++index) {
        graph.actionSlotById[graph.actions[index].id] = static_cast<uint16_t>(index + 1);
    }
    return graph;
}

piho::ActionRequest request(uint16_t actionId, uint32_t eventToken, bool sourceValue = false,
                            uint8_t targetDevice = kOutputDevice) {
    return piho::ActionRequest{kGeneration, eventToken, actionId, kSourceDevice, targetDevice,
                               sourceValue};
}

struct OutputCapture {
    uint16_t state = 0;
    uint32_t calls = 0;
    bool available = true;
};

bool captureOutput(void *context, uint8_t pin, bool value) {
    auto &capture = *static_cast<OutputCapture *>(context);
    ++capture.calls;
    if (!capture.available) {
        return false;
    }
    const uint16_t mask = static_cast<uint16_t>(1u << pin);
    capture.state = value ? static_cast<uint16_t>(capture.state | mask)
                          : static_cast<uint16_t>(capture.state & static_cast<uint16_t>(~mask));
    return true;
}

struct FrameCapture {
    static constexpr std::size_t kCapacity = 96;
    piho::CanFrame frames[kCapacity]{};
    std::size_t count = 0;
    bool result = true;
};

bool captureFrame(void *context, const piho::CanFrame &frame) {
    auto &capture = *static_cast<FrameCapture *>(context);
    if (capture.count < FrameCapture::kCapacity) {
        capture.frames[capture.count++] = frame;
    }
    return capture.result;
}

piho::ActionRequest decodeRequest(const piho::CanFrame &frame) {
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::MessageType::ExecuteAction),
                            static_cast<uint8_t>(decoded.message.type));
    return decoded.message.actionRequest;
}

piho::ActionAcknowledgement roundTripAcknowledgement(
    const piho::ActionAcknowledgement &acknowledgement) {
    piho::CanFrame frame{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::actionAcknowledgement(acknowledgement, frame));
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::MessageType::ActionAck),
                            static_cast<uint8_t>(decoded.message.type));
    return decoded.message.actionAcknowledgement;
}

void test_action_protocol_round_trips_full_generation_and_strict_payload() {
    const piho::ActionRequest original{0x89ABCDEFu, piho::kActionEventTokenMaximum, 512, 31, 7,
                                       true};
    piho::CanFrame frame{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::executeAction(original, frame));
    TEST_ASSERT_TRUE(frame.extended);
    TEST_ASSERT_FALSE(frame.remote);
    TEST_ASSERT_EQUAL_UINT8(8, frame.length);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::MessageType::ExecuteAction),
                            static_cast<uint8_t>((frame.identifier >> 8) & 0xFFu));
    TEST_ASSERT_EQUAL_UINT8(7, static_cast<uint8_t>(frame.identifier & 0xFFu));

    piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_HEX32(original.generation, decoded.message.actionRequest.generation);
    TEST_ASSERT_EQUAL_UINT32(original.eventToken, decoded.message.actionRequest.eventToken);
    TEST_ASSERT_EQUAL_UINT16(original.actionId, decoded.message.actionRequest.actionId);
    TEST_ASSERT_EQUAL_UINT8(original.sourceDevice, decoded.message.actionRequest.sourceDevice);
    TEST_ASSERT_EQUAL_UINT8(original.targetDevice, decoded.message.actionRequest.targetDevice);
    TEST_ASSERT_TRUE(decoded.message.actionRequest.sourceValue);

    const piho::ActionAcknowledgement ack{original.generation,
                                           original.eventToken,
                                           original.actionId,
                                           original.sourceDevice,
                                           original.targetDevice,
                                           piho::ActionAckStatus::WrongTarget};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::actionAcknowledgement(ack, frame));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::MessageType::ActionAck),
                            static_cast<uint8_t>((frame.identifier >> 8) & 0xFFu));
    TEST_ASSERT_LESS_THAN_UINT32(
        piho::kProtocolHeader |
            (static_cast<uint32_t>(piho::kGraphTransferMessageTypeMinimum) << 8),
        frame.identifier);
    decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ack.status),
                            static_cast<uint8_t>(decoded.message.actionAcknowledgement.status));
    TEST_ASSERT_EQUAL_UINT8(ack.sourceDevice,
                            decoded.message.actionAcknowledgement.sourceDevice);
    TEST_ASSERT_EQUAL_UINT8(ack.outputDevice,
                            decoded.message.actionAcknowledgement.outputDevice);

    piho::ActionRequest invalid = original;
    invalid.eventToken = piho::kActionEventTokenMaximum + 1;
    TEST_ASSERT_FALSE(piho::ProtocolCodec::executeAction(invalid, frame));

    TEST_ASSERT_TRUE(piho::ProtocolCodec::executeAction(original, frame));
    frame.remote = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::RemoteFrame),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));
    frame.remote = false;
    frame.length = 7;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidLength),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));
    frame.length = 8;
    frame.identifier |= 0x20u;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidDevice),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));

    TEST_ASSERT_TRUE(piho::ProtocolCodec::executeAction(original, frame));
    frame.data[0] = 0;
    frame.data[1] = 0;
    frame.data[2] = 0;
    frame.data[3] = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidPayload),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));
    TEST_ASSERT_TRUE(piho::ProtocolCodec::executeAction(original, frame));
    frame.data[4] = 0;
    frame.data[5] = 0;
    frame.data[6] = 0;
    frame.data[7] = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidPayload),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));

    TEST_ASSERT_TRUE(piho::ProtocolCodec::actionAcknowledgement(ack, frame));
    frame.identifier = (frame.identifier & ~0xFFu) | ack.sourceDevice;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidPayload),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));
    TEST_ASSERT_TRUE(piho::ProtocolCodec::actionAcknowledgement(ack, frame));
    frame.data[7] |= 0x80u;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidPayload),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));
}

void test_output_executor_applies_each_operation_once_and_services_pulses() {
    piho::LocalOutputGraph graph = outputGraph();
    OutputCapture capture{};
    piho::OutputActionExecutor executor(kOutputDevice, captureOutput, &capture);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::None),
        static_cast<uint8_t>(executor.activate(graph, 0)));

    piho::ActionAcknowledgement ack = executor.execute(request(1, 1), 10);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::Executed),
                            static_cast<uint8_t>(ack.status));
    TEST_ASSERT_EQUAL_HEX16(1, capture.state);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::AlreadyExecuted),
                            static_cast<uint8_t>(executor.execute(request(1, 1), 11).status));
    TEST_ASSERT_EQUAL_UINT32(1, capture.calls);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(2, 2, true), 12).status));
    TEST_ASSERT_EQUAL_HEX16(3, capture.state);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(3, 3), 13).status));
    TEST_ASSERT_EQUAL_HEX16(7, capture.state);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::AlreadyExecuted),
                            static_cast<uint8_t>(executor.execute(request(3, 3), 14).status));
    TEST_ASSERT_EQUAL_HEX16(7, capture.state);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(4, 4), UINT32_MAX - 50).status));
    TEST_ASSERT_EQUAL_HEX16(15, capture.state);
    TEST_ASSERT_EQUAL_UINT32(1, executor.activePulseCount());
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::AlreadyExecuted),
        static_cast<uint8_t>(executor.execute(request(4, 4), 20).status));
    TEST_ASSERT_EQUAL_UINT32(0, executor.service(48));
    TEST_ASSERT_EQUAL_UINT32(1, executor.service(49));
    TEST_ASSERT_EQUAL_HEX16(7, capture.state);
    TEST_ASSERT_EQUAL_UINT32(0, executor.activePulseCount());

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(5, 5), 60).status));
    TEST_ASSERT_EQUAL_HEX16(6, capture.state);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(4, 6), 70).status));
    TEST_ASSERT_EQUAL_UINT32(1, executor.activePulseCount());
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(6, 7), 80).status));
    TEST_ASSERT_EQUAL_UINT32(0, executor.activePulseCount());
    TEST_ASSERT_EQUAL_UINT32(0, executor.service(1000));
    TEST_ASSERT_EQUAL_HEX16(14, capture.state);
    TEST_ASSERT_EQUAL_UINT32(7, executor.counters().executed);
    TEST_ASSERT_EQUAL_UINT32(3, executor.counters().duplicates);
}

void test_output_executor_rejects_stale_unknown_wrong_target_and_unavailable_actions() {
    piho::LocalOutputGraph graph = outputGraph();
    OutputCapture capture{};
    piho::OutputActionExecutor executor(kOutputDevice, captureOutput, &capture);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::None),
        static_cast<uint8_t>(executor.activate(graph, 0)));

    piho::ActionRequest candidate = request(1, 1);
    candidate.generation = kGeneration - 1;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::WrongGeneration),
        static_cast<uint8_t>(executor.execute(candidate, 0).status));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::UnknownAction),
        static_cast<uint8_t>(executor.execute(request(20, 2), 0).status));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::WrongTarget),
        static_cast<uint8_t>(executor.execute(request(1, 3, false, 8), 0).status));
    TEST_ASSERT_EQUAL_UINT32(0, capture.calls);

    capture.available = false;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::UnavailableOutput),
        static_cast<uint8_t>(executor.execute(request(3, 4), 0).status));
    TEST_ASSERT_EQUAL_HEX16(0, executor.currentOutputs());
    capture.available = true;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(request(3, 4), 1).status));
    TEST_ASSERT_EQUAL_HEX16(4, executor.currentOutputs());

    piho::LocalOutputGraph incompatible = graph;
    incompatible.role = piho::GraphDeviceRole::Input;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::RoleMismatch),
        static_cast<uint8_t>(executor.activate(incompatible, 0)));
    TEST_ASSERT_TRUE(executor.active());
    TEST_ASSERT_EQUAL_HEX16(4, executor.currentOutputs());
}

void test_output_executor_refuses_work_when_deduplication_window_is_full() {
    piho::LocalOutputGraph graph = outputGraph();
    OutputCapture capture{};
    piho::OutputActionExecutor executor(kOutputDevice, captureOutput, &capture);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::None),
        static_cast<uint8_t>(executor.activate(graph, 0)));

    for (uint32_t token = 1; token <= piho::kActionDeduplicationCapacity; ++token) {
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::ActionAckStatus::Executed),
            static_cast<uint8_t>(executor.execute(request(1, token), 0).status));
    }
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::UnavailableOutput),
        static_cast<uint8_t>(
            executor.execute(request(1, piho::kActionDeduplicationCapacity + 1), 0).status));
    TEST_ASSERT_EQUAL_UINT32(1, executor.counters().deduplicationOverflows);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(executor.execute(
            request(1, piho::kActionDeduplicationCapacity + 1),
            piho::kActionDeduplicationWindowMs).status));
}

void test_reliable_sender_recovers_from_request_and_acknowledgement_loss() {
    piho::LocalOutputGraph graph = outputGraph();
    OutputCapture output{};
    piho::OutputActionExecutor executor(kOutputDevice, captureOutput, &output);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::None),
        static_cast<uint8_t>(executor.activate(graph, 0)));
    FrameCapture frames{};
    piho::ReliableActionSender sender(kSourceDevice, captureFrame, &frames);

    const piho::FlowActionInvocation setInvocation{kGeneration, 10, 1, kSourceDevice,
                                                   kOutputDevice, true};
    TEST_ASSERT_TRUE(sender.enqueue(setInvocation, 0));
    TEST_ASSERT_EQUAL_UINT32(1, sender.service(0));
    TEST_ASSERT_EQUAL_UINT32(0, sender.service(piho::kActionAcknowledgementTimeoutMs - 1));
    TEST_ASSERT_EQUAL_UINT32(1, sender.service(piho::kActionAcknowledgementTimeoutMs));
    TEST_ASSERT_EQUAL_UINT32(2, frames.count);

    piho::ActionAcknowledgement ack =
        executor.execute(decodeRequest(frames.frames[1]), piho::kActionAcknowledgementTimeoutMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::Executed),
                            static_cast<uint8_t>(ack.status));
    TEST_ASSERT_TRUE(sender.acknowledge(roundTripAcknowledgement(ack),
                                        piho::kActionAcknowledgementTimeoutMs));
    TEST_ASSERT_EQUAL_UINT32(0, sender.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(1, sender.counters().retries);
    TEST_ASSERT_EQUAL_UINT32(1, sender.counters().timeouts);

    const piho::FlowActionInvocation toggleInvocation{kGeneration, 11, 3, kSourceDevice,
                                                      kOutputDevice, false};
    TEST_ASSERT_TRUE(sender.enqueue(toggleInvocation, 200));
    TEST_ASSERT_EQUAL_UINT32(1, sender.service(200));
    ack = executor.execute(decodeRequest(frames.frames[2]), 200);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::Executed),
                            static_cast<uint8_t>(ack.status));

    TEST_ASSERT_EQUAL_UINT32(1, sender.service(300));
    ack = executor.execute(decodeRequest(frames.frames[3]), 300);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::AlreadyExecuted),
                            static_cast<uint8_t>(ack.status));
    TEST_ASSERT_TRUE(sender.acknowledge(roundTripAcknowledgement(ack), 300));
    TEST_ASSERT_EQUAL_UINT32(0, sender.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(2, sender.counters().retries);
    TEST_ASSERT_EQUAL_UINT32(2, sender.counters().timeouts);
    TEST_ASSERT_EQUAL_UINT32(1, sender.counters().alreadyExecutedAcknowledgements);
    TEST_ASSERT_EQUAL_UINT32(1, executor.counters().duplicates);
}

void test_reliable_sender_bounds_queue_work_and_retry_exhaustion() {
    FrameCapture frames{};
    piho::ReliableActionSender queued(kSourceDevice, captureFrame, &frames);
    for (uint32_t token = 1; token <= piho::kActionPendingCapacity; ++token) {
        const piho::FlowActionInvocation invocation{kGeneration, token, 1, kSourceDevice,
                                                    kOutputDevice, false};
        TEST_ASSERT_TRUE(queued.enqueue(invocation, 0));
    }
    const piho::FlowActionInvocation overflow{
        kGeneration, piho::kActionPendingCapacity + 1, 1, kSourceDevice, kOutputDevice, false};
    TEST_ASSERT_FALSE(queued.enqueue(overflow, 0));
    TEST_ASSERT_EQUAL_UINT32(piho::kActionPendingCapacity, queued.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(1, queued.counters().queueOverflows);
    TEST_ASSERT_EQUAL_UINT32(piho::kActionRuntimeServiceBudget, queued.service(0));

    FrameCapture failingFrames{};
    failingFrames.result = false;
    piho::ReliableActionSender exhausted(kSourceDevice, captureFrame, &failingFrames);
    const piho::FlowActionInvocation invocation{kGeneration, 100, 1, kSourceDevice,
                                                kOutputDevice, false};
    TEST_ASSERT_TRUE(exhausted.enqueue(invocation, 0));
    TEST_ASSERT_EQUAL_UINT32(1, exhausted.service(0));
    TEST_ASSERT_EQUAL_UINT32(1, exhausted.service(100));
    TEST_ASSERT_EQUAL_UINT32(1, exhausted.service(200));
    TEST_ASSERT_EQUAL_UINT32(1, exhausted.service(300));
    TEST_ASSERT_EQUAL_UINT32(0, exhausted.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(3, exhausted.counters().transmissionAttempts);
    TEST_ASSERT_EQUAL_UINT32(2, exhausted.counters().retries);
    TEST_ASSERT_EQUAL_UINT32(3, exhausted.counters().sendFailures);
    TEST_ASSERT_EQUAL_UINT32(0, exhausted.counters().timeouts);
    TEST_ASSERT_EQUAL_UINT32(1, exhausted.counters().exhausted);

    FrameCapture acceptedFrames{};
    piho::ReliableActionSender stalled(kSourceDevice, captureFrame, &acceptedFrames);
    TEST_ASSERT_TRUE(stalled.enqueue(invocation, 0));
    TEST_ASSERT_EQUAL_UINT32(1, stalled.service(0));
    TEST_ASSERT_EQUAL_UINT32(1, stalled.service(piho::kActionDeduplicationWindowMs));
    TEST_ASSERT_EQUAL_UINT32(1, acceptedFrames.count);
    TEST_ASSERT_EQUAL_UINT32(0, stalled.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(0, stalled.counters().retries);
    TEST_ASSERT_EQUAL_UINT32(1, stalled.counters().timeouts);
    TEST_ASSERT_EQUAL_UINT32(1, stalled.counters().exhausted);
}

void test_reliable_sender_surfaces_application_rejection() {
    piho::LocalOutputGraph graph = outputGraph();
    OutputCapture output{};
    piho::OutputActionExecutor executor(kOutputDevice, captureOutput, &output);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::OutputActionActivationError::None),
        static_cast<uint8_t>(executor.activate(graph, 0)));
    FrameCapture frames{};
    piho::ReliableActionSender sender(kSourceDevice, captureFrame, &frames);
    const piho::FlowActionInvocation stale{kGeneration - 1, 5, 1, kSourceDevice,
                                           kOutputDevice, false};
    TEST_ASSERT_TRUE(sender.enqueue(stale, 0));
    TEST_ASSERT_EQUAL_UINT32(1, sender.service(0));
    const piho::ActionAcknowledgement rejection = executor.execute(decodeRequest(frames.frames[0]), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ActionAckStatus::WrongGeneration),
                            static_cast<uint8_t>(rejection.status));
    TEST_ASSERT_TRUE(sender.acknowledge(roundTripAcknowledgement(rejection), 0));
    TEST_ASSERT_EQUAL_UINT32(0, sender.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(1, sender.counters().rejections);
    TEST_ASSERT_EQUAL_UINT32(1, executor.counters().wrongGenerations);
    TEST_ASSERT_EQUAL_UINT32(0, output.calls);
}

}  // namespace

void runActionRuntimeTests() {
    RUN_TEST(test_action_protocol_round_trips_full_generation_and_strict_payload);
    RUN_TEST(test_output_executor_applies_each_operation_once_and_services_pulses);
    RUN_TEST(test_output_executor_rejects_stale_unknown_wrong_target_and_unavailable_actions);
    RUN_TEST(test_output_executor_refuses_work_when_deduplication_window_is_full);
    RUN_TEST(test_reliable_sender_recovers_from_request_and_acknowledgement_loss);
    RUN_TEST(test_reliable_sender_bounds_queue_work_and_retry_exhaustion);
    RUN_TEST(test_reliable_sender_surfaces_application_rejection);
}
