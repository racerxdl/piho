#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "fake_graph_store_backend.h"
#include "piho/graph_image.h"
#include "piho/graph_update.h"

namespace {

constexpr const char *kGoldenPath = "tools/piho-flow/test/fixtures/synthetic.phg";
constexpr uint32_t kExpectedDevices =
    (static_cast<uint32_t>(1u) << 1) | (static_cast<uint32_t>(1u) << 2) |
    (static_cast<uint32_t>(1u) << 7) | (static_cast<uint32_t>(1u) << 8);

uint32_t readUint32(const uint8_t *data) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(data[byte]) << (byte * 8);
    }
    return value;
}

void writeUint32(std::vector<uint8_t> &data, std::size_t offset, uint32_t value) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        data[offset + byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

std::vector<uint8_t> graphImage(uint32_t generation) {
    std::ifstream input(kGoldenPath, std::ios::binary);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "cannot open shared synthetic.phg fixture");
    std::vector<uint8_t> image{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    writeUint32(image, 8, generation);
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::checksum(
            piho::GraphImageSource::fromMemory(image.data(), image.size()), checksum)));
    writeUint32(image, piho::kGraphChecksumOffset, checksum);
    return image;
}

piho::GraphTransferDescriptor transferDescriptor(const std::vector<uint8_t> &image,
                                                  uint16_t transferId,
                                                  uint32_t expectedDevices = kExpectedDevices) {
    return piho::GraphTransferDescriptor{
        transferId,
        piho::kGraphFormatVersion,
        piho::kGraphExecutorApiVersion,
        readUint32(&image[8]),
        static_cast<uint32_t>(image.size()),
        readUint32(&image[piho::kGraphChecksumOffset]),
        expectedDevices,
    };
}

void makeActive(piho::GraphStore &store, const std::vector<uint8_t> &image) {
    const piho::GraphReceiveDescriptor receive{
        static_cast<uint32_t>(image.size()), readUint32(&image[8]),
        readUint32(&image[piho::kGraphChecksumOffset])};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.beginReceive(receive)));
    for (std::size_t offset = 0; offset < image.size();) {
        const std::size_t size = image.size() - offset < 64 ? image.size() - offset : 64;
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphStoreError::None),
            static_cast<uint8_t>(store.writeChunk(image.data() + offset, size)));
        offset += size;
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.finishReceive()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(store.activate()));
}

struct SimulatedBus {
    static bool write(void *context, const piho::CanFrame &frame) {
        auto &bus = *static_cast<SimulatedBus *>(context);
        bus.frames.push_back(frame);
        return true;
    }

    void add(uint8_t device, piho::GraphUpdateParticipant &participant) {
        TEST_ASSERT_LESS_THAN_UINT8(4, participantCount);
        devices[participantCount] = device;
        participants[participantCount] = &participant;
        ++participantCount;
    }

    void dropNextFor(piho::MessageType type, uint8_t device) {
        dropType = type;
        dropDevice = device;
        dropPending = true;
    }

    void pump(uint32_t nowMilliseconds) {
        std::size_t index = 0;
        std::size_t delivered = 0;
        while (index < frames.size()) {
            TEST_ASSERT_LESS_THAN_UINT32(20000, delivered++);
            const piho::CanFrame frame = frames[index++];
            const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
            TEST_ASSERT_TRUE(decoded.ok());
            coordinator->handle(decoded.message, nowMilliseconds);
            for (uint8_t participant = 0; participant < participantCount; ++participant) {
                if (dropPending && decoded.message.type == dropType &&
                    devices[participant] == dropDevice) {
                    dropPending = false;
                    continue;
                }
                participants[participant]->handle(decoded.message, nowMilliseconds);
            }
        }
        frames.clear();
    }

    piho::GraphUpdateCoordinator *coordinator = nullptr;
    piho::GraphUpdateParticipant *participants[4]{};
    uint8_t devices[4]{};
    uint8_t participantCount = 0;
    std::vector<piho::CanFrame> frames{};
    piho::MessageType dropType = piho::MessageType::GraphChunk;
    uint8_t dropDevice = 0;
    bool dropPending = false;
};

void tick(SimulatedBus &bus, piho::GraphUpdateCoordinator &coordinator,
          piho::GraphUpdateParticipant &first, piho::GraphUpdateParticipant &second,
          uint32_t &nowMilliseconds) {
    nowMilliseconds += piho::kGraphUpdateRetryIntervalMs;
    coordinator.service(nowMilliseconds);
    bus.pump(nowMilliseconds);
    first.service(nowMilliseconds);
    second.service(nowMilliseconds);
    bus.pump(nowMilliseconds);
}

void waitForReady(SimulatedBus &bus, piho::GraphUpdateCoordinator &coordinator,
                  piho::GraphUpdateParticipant &first,
                  piho::GraphUpdateParticipant &second, uint32_t &nowMilliseconds,
                  uint32_t expectedDevices) {
    for (uint8_t attempt = 0;
         attempt < 32 && coordinator.status().readyDevices != expectedDevices; ++attempt) {
        tick(bus, coordinator, first, second, nowMilliseconds);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.status().lastError),
        "gateway rejected while waiting for ready devices");
    TEST_ASSERT_EQUAL_HEX32(expectedDevices, coordinator.status().readyDevices);
}

void waitForState(SimulatedBus &bus, piho::GraphUpdateCoordinator &coordinator,
                  piho::GraphUpdateParticipant &first,
                  piho::GraphUpdateParticipant &second, uint32_t &nowMilliseconds,
                  piho::GraphUpdateState state) {
    for (uint8_t attempt = 0; attempt < 32 && coordinator.status().state != state;
         ++attempt) {
        tick(bus, coordinator, first, second, nowMilliseconds);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.status().lastError),
        "gateway rejected while waiting for a state transition");
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(state),
                            static_cast<uint8_t>(coordinator.status().state));
}

void stageThroughGateway(SimulatedBus &bus, piho::GraphUpdateCoordinator &coordinator,
                         piho::GraphUpdateParticipant &first,
                         piho::GraphUpdateParticipant &second,
                         const std::vector<uint8_t> &image, uint16_t transferId,
                         uint32_t &nowMilliseconds, bool loseOneChunk) {
    const piho::GraphTransferDescriptor descriptor =
        transferDescriptor(image, transferId);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::None),
                            static_cast<uint8_t>(
                                coordinator.beginUpdate(descriptor, nowMilliseconds)));
    bus.pump(nowMilliseconds);
    waitForReady(bus, coordinator, first, second, nowMilliseconds,
                 descriptor.expectedDevices);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::NotReady),
                            static_cast<uint8_t>(coordinator.activateUpdate(nowMilliseconds)));
    const uint16_t sequenceCount = static_cast<uint16_t>(
        (image.size() + piho::kGraphChunkDataCapacity - 1) /
        piho::kGraphChunkDataCapacity);
    for (uint16_t sequence = 0; sequence < sequenceCount; ++sequence) {
        const std::size_t offset =
            static_cast<std::size_t>(sequence) * piho::kGraphChunkDataCapacity;
        const uint8_t size = static_cast<uint8_t>(
            image.size() - offset < piho::kGraphChunkDataCapacity
                ? image.size() - offset
                : piho::kGraphChunkDataCapacity);
        if (loseOneChunk && sequence == 5) {
            bus.dropNextFor(piho::MessageType::GraphChunk, 7);
        }
        TEST_ASSERT_EQUAL_UINT8(
            static_cast<uint8_t>(piho::GraphUpdateError::None),
            static_cast<uint8_t>(coordinator.queueChunk(
                transferId, sequence, image.data() + offset, size, nowMilliseconds)));
        if (sequence == 5) {
            TEST_ASSERT_EQUAL_UINT8(
                static_cast<uint8_t>(piho::GraphUpdateError::None),
                static_cast<uint8_t>(coordinator.queueChunk(
                    transferId, sequence, image.data() + offset, size, nowMilliseconds)));
        }
        bus.pump(nowMilliseconds);
        for (uint8_t attempt = 0;
             attempt < 16 && coordinator.status().chunkPending; ++attempt) {
            tick(bus, coordinator, first, second, nowMilliseconds);
        }
        TEST_ASSERT_FALSE(coordinator.status().chunkPending);
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(sequence + 1),
                                 coordinator.status().nextSequence);
    }

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(
            coordinator.finishUpdate(transferId, sequenceCount, nowMilliseconds)));
    bus.pump(nowMilliseconds);
    waitForState(bus, coordinator, first, second, nowMilliseconds,
                 piho::GraphUpdateState::Staged);
    TEST_ASSERT_EQUAL_HEX32(descriptor.expectedDevices,
                            coordinator.status().stagedDevices);
}

void test_graph_update_can_codec_is_strict_and_low_priority() {
    const std::vector<uint8_t> image = graphImage(1);
    const piho::GraphTransferDescriptor descriptor = transferDescriptor(image, 0x1234);
    piho::CanFrame frame{};

    TEST_ASSERT_TRUE(piho::ProtocolCodec::graphBegin(descriptor, frame));
    piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT16(descriptor.transferId,
                             decoded.message.graph.descriptor.transferId);
    TEST_ASSERT_EQUAL_UINT32(descriptor.generation,
                             decoded.message.graph.descriptor.generation);
    TEST_ASSERT_EQUAL_UINT32(descriptor.imageSize,
                             decoded.message.graph.descriptor.imageSize);

    TEST_ASSERT_TRUE(piho::ProtocolCodec::graphCompatibility(descriptor, frame));
    decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT16(descriptor.format,
                             decoded.message.graph.descriptor.format);
    TEST_ASSERT_EQUAL_UINT16(descriptor.executorApi,
                             decoded.message.graph.descriptor.executorApi);
    frame.data[7] = 1;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidPayload),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));

    TEST_ASSERT_TRUE(piho::ProtocolCodec::graphDevices(descriptor, frame));
    frame.identifier = (frame.identifier & ~0xFFu) | 1u;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidDevice),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));

    const uint8_t data[] = {0x10, 0x20, 0x30, 0x40};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::graphChunk(descriptor.transferId, 9, data,
                                                     sizeof(data), frame));
    decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT16(9, decoded.message.graph.sequence);
    TEST_ASSERT_EQUAL_UINT8(sizeof(data), decoded.message.graph.chunkSize);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, decoded.message.graph.chunk, sizeof(data));
    frame.length = 4;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidLength),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(frame).error));

    piho::CanFrame action{};
    const piho::ActionRequest request{1, 1, 1, 1, 7, true};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::executeAction(request, action));
    TEST_ASSERT_TRUE(piho::ProtocolCodec::graphChunk(descriptor.transferId, 9, data,
                                                     sizeof(data), frame));
    TEST_ASSERT_LESS_THAN_UINT32(frame.identifier, action.identifier);
}

void test_gateway_stages_activates_retries_and_rolls_back_all_boards() {
    FakeGraphStoreBackend firstBackend;
    FakeGraphStoreBackend secondBackend;
    FakeGraphStoreBackend thirdBackend;
    FakeGraphStoreBackend fourthBackend;
    piho::GraphStore firstStore(firstBackend);
    piho::GraphStore secondStore(secondBackend);
    piho::GraphStore thirdStore(thirdBackend);
    piho::GraphStore fourthStore(fourthBackend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(firstStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(secondStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(thirdStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(fourthStore.begin()));

    piho::GraphUpdateCoordinator coordinator;
    piho::GraphUpdateParticipant firstParticipant;
    piho::GraphUpdateParticipant secondParticipant;
    piho::GraphUpdateParticipant thirdParticipant;
    piho::GraphUpdateParticipant fourthParticipant;
    SimulatedBus bus;
    bus.coordinator = &coordinator;
    bus.add(1, firstParticipant);
    bus.add(7, secondParticipant);
    bus.add(2, thirdParticipant);
    bus.add(8, fourthParticipant);
    coordinator.configure(SimulatedBus::write, &bus);
    uint32_t now = 0;
    TEST_ASSERT_TRUE(firstParticipant.begin(1, firstStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(secondParticipant.begin(7, secondStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(thirdParticipant.begin(2, thirdStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(fourthParticipant.begin(8, fourthStore, SimulatedBus::write, &bus, now));

    const std::vector<uint8_t> first = graphImage(1);
    stageThroughGateway(bus, coordinator, firstParticipant, secondParticipant, first,
                        0x101, now, true);
    TEST_ASSERT_FALSE(firstStore.hasActiveGraph());
    TEST_ASSERT_FALSE(secondStore.hasActiveGraph());
    TEST_ASSERT_FALSE(thirdStore.hasActiveGraph());
    TEST_ASSERT_FALSE(fourthStore.hasActiveGraph());

    bus.dropNextFor(piho::MessageType::GraphActivate, 7);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::None),
                            static_cast<uint8_t>(coordinator.activateUpdate(now)));
    bus.pump(now);
    TEST_ASSERT_TRUE(firstStore.hasActiveGraph());
    TEST_ASSERT_FALSE(secondStore.hasActiveGraph());
    TEST_ASSERT_TRUE(thirdStore.hasActiveGraph());
    TEST_ASSERT_TRUE(fourthStore.hasActiveGraph());
    waitForState(bus, coordinator, firstParticipant, secondParticipant, now,
                 piho::GraphUpdateState::Active);
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, fourthStore.status().active.generation);
    TEST_ASSERT_GREATER_THAN_UINT32(0, firstParticipant.counters().duplicateChunks);

    const std::vector<uint8_t> second = graphImage(2);
    stageThroughGateway(bus, coordinator, firstParticipant, secondParticipant, second,
                        0x102, now, false);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::None),
                            static_cast<uint8_t>(coordinator.activateUpdate(now)));
    bus.pump(now);
    waitForState(bus, coordinator, firstParticipant, secondParticipant, now,
                 piho::GraphUpdateState::Active);
    TEST_ASSERT_EQUAL_UINT32(2, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().rollback.generation);
    TEST_ASSERT_EQUAL_UINT32(2, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(2, fourthStore.status().active.generation);

    const piho::GraphIdentity rollbackTarget{
        piho::kGraphFormatVersion, piho::kGraphExecutorApiVersion, 1,
        readUint32(&first[piho::kGraphChecksumOffset])};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.rollbackUpdate(rollbackTarget, now)));
    bus.pump(now);
    waitForState(bus, coordinator, firstParticipant, secondParticipant, now,
                 piho::GraphUpdateState::Rollback);
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, fourthStore.status().active.generation);
    TEST_ASSERT_EQUAL_HEX32(kExpectedDevices, coordinator.status().rollbackDevices);
}

void test_gateway_reports_rejection_abort_missing_devices_and_timeout() {
    FakeGraphStoreBackend firstBackend;
    FakeGraphStoreBackend secondBackend;
    FakeGraphStoreBackend thirdBackend;
    FakeGraphStoreBackend fourthBackend;
    piho::GraphStore firstStore(firstBackend);
    piho::GraphStore secondStore(secondBackend);
    piho::GraphStore thirdStore(thirdBackend);
    piho::GraphStore fourthStore(fourthBackend);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(firstStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(secondStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(thirdStore.begin()));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphStoreError::None),
                            static_cast<uint8_t>(fourthStore.begin()));
    const std::vector<uint8_t> active = graphImage(1);
    const std::vector<uint8_t> update = graphImage(2);
    makeActive(firstStore, active);
    makeActive(secondStore, active);
    makeActive(thirdStore, active);
    makeActive(fourthStore, active);

    piho::GraphUpdateCoordinator coordinator;
    piho::GraphUpdateParticipant firstParticipant;
    piho::GraphUpdateParticipant secondParticipant;
    piho::GraphUpdateParticipant thirdParticipant;
    piho::GraphUpdateParticipant fourthParticipant;
    SimulatedBus bus;
    bus.coordinator = &coordinator;
    bus.add(1, firstParticipant);
    bus.add(7, secondParticipant);
    bus.add(2, thirdParticipant);
    bus.add(8, fourthParticipant);
    coordinator.configure(SimulatedBus::write, &bus);
    uint32_t now = 0;
    TEST_ASSERT_TRUE(firstParticipant.begin(1, firstStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(secondParticipant.begin(7, secondStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(thirdParticipant.begin(2, thirdStore, SimulatedBus::write, &bus, now));
    TEST_ASSERT_TRUE(fourthParticipant.begin(8, fourthStore, SimulatedBus::write, &bus, now));

    const piho::GraphTransferDescriptor abortDescriptor =
        transferDescriptor(update, 0x201);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.beginUpdate(abortDescriptor, now)));
    bus.pump(now);
    waitForReady(bus, coordinator, firstParticipant, secondParticipant, now,
                 abortDescriptor.expectedDevices);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(
            coordinator.queueChunk(abortDescriptor.transferId, 0, update.data(), 4, now)));
    bus.pump(now);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.abortUpdate(abortDescriptor.transferId, now)));
    bus.pump(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::Aborted),
                            static_cast<uint8_t>(coordinator.status().lastError));
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, fourthStore.status().active.generation);
    TEST_ASSERT_FALSE(firstStore.hasStagedGraph());
    TEST_ASSERT_FALSE(secondStore.hasStagedGraph());
    TEST_ASSERT_FALSE(thirdStore.hasStagedGraph());
    TEST_ASSERT_FALSE(fourthStore.hasStagedGraph());

    firstBackend.failNextAppend = false;
    secondBackend.failNextAppend = true;
    const piho::GraphTransferDescriptor rejectedDescriptor =
        transferDescriptor(update, 0x202);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.beginUpdate(rejectedDescriptor, now)));
    bus.pump(now);
    waitForReady(bus, coordinator, firstParticipant, secondParticipant, now,
                 rejectedDescriptor.expectedDevices);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(
            coordinator.queueChunk(rejectedDescriptor.transferId, 0, update.data(), 4, now)));
    bus.pump(now);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateState::Rejected),
                            static_cast<uint8_t>(coordinator.status().state));
    TEST_ASSERT_BITS_HIGH(static_cast<uint32_t>(1u) << 7,
                          coordinator.status().rejectedDevices);
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, fourthStore.status().active.generation);

    const uint32_t missingDevice = static_cast<uint32_t>(1u) << 9;
    const piho::GraphTransferDescriptor timeoutDescriptor =
        transferDescriptor(update, 0x203, kExpectedDevices | missingDevice);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphUpdateError::None),
        static_cast<uint8_t>(coordinator.beginUpdate(timeoutDescriptor, now)));
    bus.pump(now);
    for (uint16_t attempt = 0; attempt < 120 &&
                                   coordinator.status().lastError != piho::GraphUpdateError::Timeout;
         ++attempt) {
        tick(bus, coordinator, firstParticipant, secondParticipant, now);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphUpdateError::Timeout),
                            static_cast<uint8_t>(coordinator.status().lastError));
    TEST_ASSERT_BITS_HIGH(missingDevice, coordinator.status().missingDevices);
    TEST_ASSERT_EQUAL_UINT32(1, firstStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, secondStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, thirdStore.status().active.generation);
    TEST_ASSERT_EQUAL_UINT32(1, fourthStore.status().active.generation);
    TEST_ASSERT_FALSE(firstStore.hasStagedGraph());
    TEST_ASSERT_FALSE(secondStore.hasStagedGraph());
    TEST_ASSERT_FALSE(thirdStore.hasStagedGraph());
    TEST_ASSERT_FALSE(fourthStore.hasStagedGraph());
}

}  // namespace

void runGraphUpdateTests() {
    RUN_TEST(test_graph_update_can_codec_is_strict_and_low_priority);
    RUN_TEST(test_gateway_stages_activates_retries_and_rolls_back_all_boards);
    RUN_TEST(test_gateway_reports_rejection_abort_missing_devices_and_timeout);
}
