#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

#include "fake_graph_store_backend.h"
#include "piho/protocol.h"
#include "piho/runtime.h"

namespace {

constexpr const char *kGoldenPath =
    "tools/piho-flow/test/fixtures/synthetic.phg";
constexpr uint8_t kInputDevice = 1;
constexpr uint8_t kFirstOutputDevice = 7;
constexpr uint8_t kSecondOutputDevice = 8;
constexpr uint16_t kInputMask = 1u << 2;

uint32_t readUint32(const uint8_t *data) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(data[byte]) << (byte * 8);
    }
    return value;
}

void writeUint16(std::vector<uint8_t> &data, std::size_t offset,
                 uint16_t value) {
    data[offset] = static_cast<uint8_t>(value & 0xFFu);
    data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void writeUint32(std::vector<uint8_t> &data, std::size_t offset,
                 uint32_t value) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        data[offset + byte] =
            static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

std::vector<uint8_t> loadImage() {
    std::ifstream input(kGoldenPath, std::ios::binary);
    TEST_ASSERT_TRUE_MESSAGE(input.good(),
                             "cannot open shared synthetic.phg fixture");
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> liveUpdateImage(const std::vector<uint8_t> &active) {
    std::vector<uint8_t> updated = active;
    writeUint32(updated, 8, 2);
    const uint32_t inputOffset = readUint32(&updated[36]);
    writeUint16(updated, inputOffset + 4, 5);
    const uint32_t referenceOffset = readUint32(&updated[44]);
    writeUint16(updated, referenceOffset + 2, 3);
    writeUint16(updated, referenceOffset + 4, 4);
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::checksum(
            piho::GraphImageSource::fromMemory(updated.data(), updated.size()),
            checksum)));
    writeUint32(updated, piho::kGraphChecksumOffset, checksum);
    return updated;
}

void stageAndActivate(piho::GraphStore &store,
                      const std::vector<uint8_t> &image) {
    const piho::GraphReceiveDescriptor descriptor{
        static_cast<uint32_t>(image.size()), readUint32(&image[8]),
        readUint32(&image[piho::kGraphChecksumOffset])};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphStoreError::None),
        static_cast<uint8_t>(store.beginReceive(descriptor)));
    for (std::size_t offset = 0; offset < image.size();) {
        const std::size_t remaining = image.size() - offset;
        const std::size_t chunk = remaining < 41 ? remaining : 41;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(
                store.writeChunk(image.data() + offset, chunk)));
        offset += chunk;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.finishReceive()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.activate()));
}

void makeActive(piho::GraphStore &store, const std::vector<uint8_t> &image) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.begin()));
    stageAndActivate(store, image);
}

struct FrameQueue {
    piho::CanFrame frames[16]{};
    std::size_t count = 0;
};

bool captureFrame(void *context, const piho::CanFrame &frame) {
    auto &queue = *static_cast<FrameQueue *>(context);
    if (queue.count >= 16) {
        return false;
    }
    queue.frames[queue.count++] = frame;
    return true;
}

struct OutputCapture {
    uint16_t state = 0;
    uint32_t writes = 0;
};

bool captureOutput(void *context, uint8_t pin, bool value) {
    auto &capture = *static_cast<OutputCapture *>(context);
    ++capture.writes;
    const uint16_t mask = static_cast<uint16_t>(1u << pin);
    capture.state = value ? static_cast<uint16_t>(capture.state | mask)
                          : static_cast<uint16_t>(capture.state & ~mask);
    return true;
}

piho::ActionRequest decodeRequest(const piho::CanFrame &frame) {
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::MessageType::ExecuteAction),
        static_cast<uint8_t>(decoded.message.type));
    return decoded.message.actionRequest;
}

void acknowledge(piho::InputGraphRuntime &input,
                 const piho::ActionAcknowledgement &acknowledgement,
                 uint32_t nowMilliseconds) {
    piho::CanFrame frame{};
    TEST_ASSERT_TRUE(
        piho::ProtocolCodec::actionAcknowledgement(acknowledgement, frame));
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_TRUE(input.acknowledge(decoded.message.actionAcknowledgement,
                                       nowMilliseconds));
}

void executeFrame(piho::InputGraphRuntime &input,
                  piho::OutputGraphRuntime &firstOutput,
                  piho::OutputGraphRuntime &secondOutput,
                  const piho::CanFrame &frame, uint32_t nowMilliseconds) {
    const piho::ActionRequest request = decodeRequest(frame);
    piho::ActionAcknowledgement result{};
    if (request.targetDevice == kFirstOutputDevice) {
        result = firstOutput.execute(request, nowMilliseconds);
    } else {
        TEST_ASSERT_EQUAL_UINT8(kSecondOutputDevice, request.targetDevice);
        result = secondOutput.execute(request, nowMilliseconds);
    }
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::Executed),
        static_cast<uint8_t>(result.status));
    acknowledge(input, result, nowMilliseconds);
}

void assertSameIdentity(const piho::GraphRuntimeStatus &left,
                        const piho::GraphRuntimeStatus &right) {
    TEST_ASSERT_NOT_EQUAL(0, left.identity.generation);
    TEST_ASSERT_EQUAL_UINT32(left.identity.generation,
                             right.identity.generation);
    TEST_ASSERT_EQUAL_HEX32(left.identity.checksum, right.identity.checksum);
}

void test_runtime_routes_source_edges_to_two_remote_outputs() {
    const std::vector<uint8_t> image = loadImage();
    FakeGraphStoreBackend inputBackend;
    FakeGraphStoreBackend firstOutputBackend;
    FakeGraphStoreBackend secondOutputBackend;
    piho::GraphStore inputStore(inputBackend);
    piho::GraphStore firstOutputStore(firstOutputBackend);
    piho::GraphStore secondOutputStore(secondOutputBackend);
    makeActive(inputStore, image);
    makeActive(firstOutputStore, image);
    makeActive(secondOutputStore, image);

    FrameQueue network;
    OutputCapture firstCapture;
    OutputCapture secondCapture;
    piho::InputGraphRuntime input(kInputDevice, inputStore, captureFrame,
                                  &network);
    piho::OutputGraphRuntime firstOutput(kFirstOutputDevice, firstOutputStore,
                                         captureOutput, &firstCapture);
    piho::OutputGraphRuntime secondOutput(
        kSecondOutputDevice, secondOutputStore, captureOutput, &secondCapture);

    TEST_ASSERT_TRUE(input.activate(0));
    TEST_ASSERT_TRUE(firstOutput.activate(0));
    TEST_ASSERT_TRUE(secondOutput.activate(0));
    TEST_ASSERT_EQUAL_UINT16(25, input.activeGraph()->inputs[0].debounceMs);

    input.service(0);
    TEST_ASSERT_EQUAL_UINT32(0, network.count);
    TEST_ASSERT_EQUAL_UINT32(0, input.status().flowAcceptedEvents);

    const piho::GraphRuntimeStatus inputIdentity = input.status();
    assertSameIdentity(inputIdentity, firstOutput.status());
    assertSameIdentity(inputIdentity, secondOutput.status());
    TEST_ASSERT_EQUAL_UINT32(inputStore.status().active.generation,
                             inputIdentity.identity.generation);
    TEST_ASSERT_EQUAL_HEX32(inputStore.status().active.checksum,
                            inputIdentity.identity.checksum);

    TEST_ASSERT_TRUE(input.submitInput(
        piho::FlowInputUpdate{kInputMask, kInputMask, kInputMask, 0, 100}));
    input.service(100);
    TEST_ASSERT_EQUAL_UINT32(1, network.count);
    TEST_ASSERT_EQUAL_UINT8(kFirstOutputDevice,
                            decodeRequest(network.frames[0]).targetDevice);

    input.service(199);
    TEST_ASSERT_EQUAL_UINT32(1, network.count);
    input.service(200);
    TEST_ASSERT_EQUAL_UINT32(2, network.count);
    executeFrame(input, firstOutput, secondOutput, network.frames[1], 200);
    TEST_ASSERT_EQUAL_HEX16(1u << 3, firstCapture.state);
    TEST_ASSERT_EQUAL_HEX16(0, secondCapture.state);
    TEST_ASSERT_EQUAL_UINT32(1, input.status().actionRetries);

    input.service(60099);
    TEST_ASSERT_EQUAL_UINT32(2, network.count);
    input.service(60100);
    TEST_ASSERT_EQUAL_UINT32(3, network.count);
    executeFrame(input, firstOutput, secondOutput, network.frames[2], 60100);
    TEST_ASSERT_EQUAL_HEX16(1u << 4, secondCapture.state);
    TEST_ASSERT_EQUAL_UINT32(1, firstOutput.status().executorExecutedActions);
    TEST_ASSERT_EQUAL_UINT32(1, secondOutput.status().executorExecutedActions);
    TEST_ASSERT_EQUAL_UINT32(1, input.status().flowAcceptedEvents);
    TEST_ASSERT_EQUAL_UINT32(2, input.status().flowEvaluatedActions);
    TEST_ASSERT_EQUAL_UINT32(0, firstOutput.status().flowAcceptedEvents);

    piho::ActionRequest rejected = decodeRequest(network.frames[1]);
    ++rejected.generation;
    const piho::ActionAcknowledgement rejection =
        firstOutput.execute(rejected, 61000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::ActionAckStatus::WrongGeneration),
        static_cast<uint8_t>(rejection.status));
    TEST_ASSERT_EQUAL_UINT32(1, firstOutput.status().executorRejectedActions);

    TEST_ASSERT_TRUE(input.submitInput(
        piho::FlowInputUpdate{0, kInputMask, 0, kInputMask, 70000}));
    input.service(70000);
    input.service(70249);
    TEST_ASSERT_EQUAL_UINT32(3, network.count);
    input.service(70250);
    TEST_ASSERT_EQUAL_UINT32(4, network.count);
    executeFrame(input, firstOutput, secondOutput, network.frames[3], 70250);
    TEST_ASSERT_EQUAL_HEX16((1u << 3) | (1u << 9), firstCapture.state);

    TEST_ASSERT_EQUAL_UINT32(0, firstOutput.service(71249));
    TEST_ASSERT_EQUAL_HEX16((1u << 3) | (1u << 9), firstCapture.state);
    TEST_ASSERT_EQUAL_UINT32(1, firstOutput.service(71250));
    TEST_ASSERT_EQUAL_HEX16(1u << 3, firstCapture.state);
    TEST_ASSERT_EQUAL_UINT32(2, firstOutput.status().executorExecutedActions);
    TEST_ASSERT_EQUAL_UINT32(3, input.status().flowEvaluatedActions);

    const std::vector<uint8_t> updated = liveUpdateImage(image);
    stageAndActivate(inputStore, updated);
    stageAndActivate(firstOutputStore, updated);
    stageAndActivate(secondOutputStore, updated);
    const std::size_t framesBeforeActivation = network.count;
    TEST_ASSERT_TRUE(input.activate(0));
    TEST_ASSERT_TRUE(firstOutput.activate(firstCapture.state));
    TEST_ASSERT_TRUE(secondOutput.activate(secondCapture.state));
    TEST_ASSERT_EQUAL_UINT32(2, input.status().identity.generation);
    TEST_ASSERT_EQUAL_UINT16(5, input.activeGraph()->inputs[0].debounceMs);
    assertSameIdentity(input.status(), firstOutput.status());
    assertSameIdentity(input.status(), secondOutput.status());

    input.service(80000);
    TEST_ASSERT_EQUAL_UINT32(framesBeforeActivation, network.count);
    TEST_ASSERT_TRUE(input.submitInput(
        piho::FlowInputUpdate{kInputMask, kInputMask, kInputMask, 0, 81000}));
    input.service(81000);
    TEST_ASSERT_EQUAL_UINT32(framesBeforeActivation, network.count);
    input.service(81249);
    TEST_ASSERT_EQUAL_UINT32(framesBeforeActivation, network.count);
    input.service(81250);
    TEST_ASSERT_EQUAL_UINT32(framesBeforeActivation + 1, network.count);
    const piho::ActionRequest updatedRequest =
        decodeRequest(network.frames[framesBeforeActivation]);
    TEST_ASSERT_EQUAL_UINT16(3, updatedRequest.actionId);
    TEST_ASSERT_EQUAL_UINT8(kFirstOutputDevice, updatedRequest.targetDevice);
}

}  // namespace

void runRuntimeIntegrationTests() {
    RUN_TEST(test_runtime_routes_source_edges_to_two_remote_outputs);
}
