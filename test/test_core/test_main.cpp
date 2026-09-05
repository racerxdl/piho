#include <unity.h>

#include <cstddef>

#include "piho.h"

#include "piho/addressing.h"
#include "piho/debouncer.h"
#include "piho/protocol.h"
#include "piho/serial_framer.h"
#include "piho/trigger_table.h"
#include "piho/trigger_storage_codec.h"

void runGraphImageTests();
void runFlowEngineTests();
void runActionRuntimeTests();
void runGraphStoreTests();

namespace {

class FakeTransport final : public CanTransport {
   public:
    bool begin() override {
        began = true;
        return beginResult;
    }

    void poll() override { ++pollCalls; }

    bool trySend(const piho::CanFrame &frame) override {
        if (!sendResult || sentCount >= kCapacity) {
            ++currentStats.txDropped;
            return false;
        }
        sent[sentCount++] = frame;
        return true;
    }

    bool tryReceive(piho::CanFrame &frame) override {
        if (receivedRead == receivedCount) {
            return false;
        }
        frame = received[receivedRead++];
        return true;
    }

    CanTransportStats stats() const override { return currentStats; }

    void enqueue(const piho::CanFrame &frame) {
        TEST_ASSERT_LESS_THAN_UINT32(kCapacity, receivedCount);
        received[receivedCount++] = frame;
    }

    static constexpr std::size_t kCapacity = 8;
    piho::CanFrame sent[kCapacity]{};
    piho::CanFrame received[kCapacity]{};
    CanTransportStats currentStats{};
    std::size_t sentCount = 0;
    std::size_t receivedCount = 0;
    std::size_t receivedRead = 0;
    uint32_t pollCalls = 0;
    bool beginResult = true;
    bool sendResult = true;
    bool began = false;
};

struct ControllerCapture {
    uint32_t pinCalls = 0;
    uint32_t byteCalls = 0;
    uint32_t inputCalls = 0;
    uint32_t errorCalls = 0;
    uint8_t localPin = 0;
    uint8_t localByte = 0;
    uint8_t inputDevice = 0;
    uint16_t inputState = 0;
    uint8_t byteValue = 0;
    bool pinValue = false;
    ControllerError lastError = ControllerError::InvalidFrame;
};

void capturePin(void *context, uint8_t localPin, bool value) {
    auto &capture = *static_cast<ControllerCapture *>(context);
    ++capture.pinCalls;
    capture.localPin = localPin;
    capture.pinValue = value;
}

void captureByte(void *context, uint8_t localByte, uint8_t value) {
    auto &capture = *static_cast<ControllerCapture *>(context);
    ++capture.byteCalls;
    capture.localByte = localByte;
    capture.byteValue = value;
}

void captureInput(void *context, uint8_t device, uint16_t state) {
    auto &capture = *static_cast<ControllerCapture *>(context);
    ++capture.inputCalls;
    capture.inputDevice = device;
    capture.inputState = state;
}

void captureError(void *context, ControllerError error) {
    auto &capture = *static_cast<ControllerCapture *>(context);
    ++capture.errorCalls;
    capture.lastError = error;
}

PihoCallbacks captureCallbacks(ControllerCapture &capture) {
    PihoCallbacks callbacks{};
    callbacks.context = &capture;
    callbacks.onSetPin = capturePin;
    callbacks.onSetByte = captureByte;
    callbacks.onInputState = captureInput;
    callbacks.onError = captureError;
    return callbacks;
}

void test_global_addresses_cover_every_device() {
    piho::PinAddress pin{};
    TEST_ASSERT_TRUE(piho::decodeGlobalPin(16, pin));
    TEST_ASSERT_EQUAL_UINT8(1, pin.device);
    TEST_ASSERT_EQUAL_UINT8(0, pin.localPin);

    TEST_ASSERT_TRUE(piho::decodeGlobalPin(511, pin));
    TEST_ASSERT_EQUAL_UINT8(31, pin.device);
    TEST_ASSERT_EQUAL_UINT8(15, pin.localPin);
    TEST_ASSERT_FALSE(piho::decodeGlobalPin(512, pin));

    piho::ByteAddress byte{};
    TEST_ASSERT_TRUE(piho::decodeGlobalByte(2, byte));
    TEST_ASSERT_EQUAL_UINT8(1, byte.device);
    TEST_ASSERT_EQUAL_UINT8(0, byte.localByte);
    TEST_ASSERT_FALSE(piho::decodeGlobalByte(64, byte));
}

void test_set_pin_value_is_inside_payload() {
    piho::CanFrame frame{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::setPin(1, 3, true, frame));
    TEST_ASSERT_TRUE(frame.extended);
    TEST_ASSERT_FALSE(frame.remote);
    TEST_ASSERT_EQUAL_UINT8(2, frame.length);
    TEST_ASSERT_EQUAL_UINT8(3, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT8(1, frame.data[1]);

    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::MessageType::SetPin),
                            static_cast<uint8_t>(decoded.message.type));
    TEST_ASSERT_EQUAL_UINT8(1, decoded.message.device);
    TEST_ASSERT_EQUAL_UINT8(3, decoded.message.index);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.message.value);
}

void test_protocol_rejects_unrelated_and_malformed_frames() {
    piho::CanFrame standardReset{};
    standardReset.identifier = 0x3FF;
    standardReset.extended = false;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::NotExtended),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(standardReset).error));

    piho::CanFrame wrongNamespace = piho::ProtocolCodec::reset();
    wrongNamespace.identifier ^= 1u << 20;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidIdentifier),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(wrongNamespace).error));
    wrongNamespace.remote = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidIdentifier),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(wrongNamespace).error));

    piho::CanFrame remoteReset = piho::ProtocolCodec::reset();
    remoteReset.remote = true;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::RemoteFrame),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(remoteReset).error));

    piho::CanFrame shortOutput{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::outputState(2, 0xA55A, shortOutput));
    shortOutput.length = 1;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::ProtocolError::InvalidLength),
                            static_cast<uint8_t>(piho::ProtocolCodec::decode(shortOutput).error));
}

void test_input_and_output_state_are_little_endian() {
    piho::CanFrame frame{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::inputState(7, 0xA55A, frame));
    TEST_ASSERT_EQUAL_HEX8(0x5A, frame.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA5, frame.data[1]);

    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(frame);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_HEX16(0xA55A, decoded.message.state);
}

void test_trigger_router_only_toggles_on_rising_edges() {
    piho::TriggerTable table;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::TriggerUpdateResult::Inserted),
                            static_cast<uint8_t>(table.upsert(piho::TriggerRule{1, 2, 7})));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::TriggerUpdateResult::AlreadyPresent),
                            static_cast<uint8_t>(table.upsert(piho::TriggerRule{1, 2, 7})));

    piho::TriggerRouter router;
    TEST_ASSERT_EQUAL_HEX16(0, router.update(1, 0, table));
    TEST_ASSERT_EQUAL_HEX16(1u << 7, router.update(1, 1u << 2, table));
    TEST_ASSERT_EQUAL_HEX16(0, router.update(1, 1u << 2, table));
    TEST_ASSERT_EQUAL_HEX16(0, router.update(1, 0, table));
    TEST_ASSERT_EQUAL_HEX16(1u << 7, router.update(1, 1u << 2, table));
}

void test_debouncer_requires_a_stable_candidate() {
    piho::InputDebouncer debouncer(100);
    piho::DebounceUpdate update = debouncer.update(0, 0);
    TEST_ASSERT_TRUE(update.initialized);
    TEST_ASSERT_EQUAL_HEX16(0, update.state);

    update = debouncer.update(1, 10);
    TEST_ASSERT_EQUAL_HEX16(0, update.state);
    update = debouncer.update(0, 20);
    TEST_ASSERT_EQUAL_HEX16(0, update.state);

    update = debouncer.update(1, 30);
    TEST_ASSERT_EQUAL_HEX16(0, update.state);
    update = debouncer.update(1, 129);
    TEST_ASSERT_EQUAL_HEX16(0, update.state);
    update = debouncer.update(1, 130);
    TEST_ASSERT_EQUAL_HEX16(1, update.state);
    TEST_ASSERT_EQUAL_HEX16(1, update.changed);
}

void test_debouncer_handles_millisecond_wraparound() {
    piho::InputDebouncer debouncer(100);
    debouncer.update(0, UINT32_MAX - 100);
    debouncer.update(1, UINT32_MAX - 50);
    const piho::DebounceUpdate update = debouncer.update(1, 49);
    TEST_ASSERT_EQUAL_HEX16(1, update.state);
    TEST_ASSERT_EQUAL_HEX16(1, update.changed);
}

void test_serial_frames_recover_from_noise_and_bad_checksum() {
    const uint8_t payload[] = {0x08, 0x96, 0x01};
    uint8_t encoded[piho::kSerialFrameCapacity]{};
    std::size_t encodedLength = 0;
    TEST_ASSERT_TRUE(piho::SerialFrameEncoder::encode(payload, sizeof(payload), encoded, sizeof(encoded),
                                                      encodedLength));

    piho::SerialFrameParser parser;
    piho::SerialFrameView frame{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FrameParseStatus::None),
                            static_cast<uint8_t>(parser.push(0xAA, frame)));

    uint8_t corrupt[piho::kSerialFrameCapacity]{};
    for (std::size_t index = 0; index < encodedLength; ++index) {
        corrupt[index] = encoded[index];
    }
    corrupt[5] ^= 0x01;

    piho::FrameParseStatus status = piho::FrameParseStatus::None;
    for (std::size_t index = 0; index < encodedLength; ++index) {
        status = parser.push(corrupt[index], frame);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FrameParseStatus::InvalidChecksum),
                            static_cast<uint8_t>(status));

    for (std::size_t index = 0; index < encodedLength; ++index) {
        status = parser.push(encoded[index], frame);
    }
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::FrameParseStatus::Complete),
                            static_cast<uint8_t>(status));
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), frame.length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, frame.data, sizeof(payload));
}

void test_trigger_storage_round_trips_and_rejects_corruption() {
    piho::TriggerTable source;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::TriggerUpdateResult::Inserted),
                            static_cast<uint8_t>(source.upsert(piho::TriggerRule{1, 2, 3})));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::TriggerUpdateResult::Inserted),
                            static_cast<uint8_t>(source.upsert(piho::TriggerRule{4, 5, 6})));

    uint8_t image[piho::kTriggerStorageCapacity]{};
    std::size_t imageSize = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::TriggerStorageError::None),
        static_cast<uint8_t>(piho::TriggerStorageCodec::encode(source, 42, image, sizeof(image), imageSize)));

    piho::TriggerTable decoded;
    uint32_t generation = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::TriggerStorageError::None),
        static_cast<uint8_t>(piho::TriggerStorageCodec::decode(image, imageSize, decoded, generation)));
    TEST_ASSERT_EQUAL_UINT32(42, generation);
    TEST_ASSERT_EQUAL_UINT32(2, decoded.size());
    const piho::TriggerRule firstExpected{1, 2, 3};
    const piho::TriggerRule secondExpected{4, 5, 6};
    TEST_ASSERT_TRUE(decoded.at(0) == firstExpected);
    TEST_ASSERT_TRUE(decoded.at(1) == secondExpected);

    image[12] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::TriggerStorageError::InvalidChecksum),
        static_cast<uint8_t>(piho::TriggerStorageCodec::decode(image, imageSize, decoded, generation)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::TriggerStorageError::InvalidLength),
        static_cast<uint8_t>(piho::TriggerStorageCodec::decode(image, imageSize - 1, decoded, generation)));
}


void test_controller_maps_global_outputs_and_avoids_self_echo() {
    FakeTransport transport;
    ControllerCapture capture;
    PihoController controller(transport);
    controller.setCallbacks(captureCallbacks(capture));
    TEST_ASSERT_TRUE(controller.begin(2));
    TEST_ASSERT_TRUE(transport.began);

    TEST_ASSERT_TRUE(controller.setGlobalPin(47, true));
    TEST_ASSERT_EQUAL_UINT32(1, capture.pinCalls);
    TEST_ASSERT_EQUAL_UINT8(15, capture.localPin);
    TEST_ASSERT_TRUE(capture.pinValue);
    TEST_ASSERT_EQUAL_UINT32(0, transport.sentCount);

    TEST_ASSERT_TRUE(controller.setGlobalByte(5, 0xA5));
    TEST_ASSERT_EQUAL_UINT32(1, capture.byteCalls);
    TEST_ASSERT_EQUAL_UINT8(1, capture.localByte);
    TEST_ASSERT_EQUAL_HEX8(0xA5, capture.byteValue);
    TEST_ASSERT_EQUAL_UINT32(0, transport.sentCount);

    TEST_ASSERT_TRUE(controller.setGlobalPin(52, false));
    TEST_ASSERT_EQUAL_UINT32(1, transport.sentCount);
    const piho::DecodeResult decoded = piho::ProtocolCodec::decode(transport.sent[0]);
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_UINT8(3, decoded.message.device);
    TEST_ASSERT_EQUAL_UINT8(4, decoded.message.index);
    TEST_ASSERT_EQUAL_UINT8(0, decoded.message.value);

    FakeTransport inputTransport;
    ControllerCapture inputCapture;
    PihoController inputController(inputTransport);
    PihoCallbacks inputCallbacks{};
    inputCallbacks.context = &inputCapture;
    inputCallbacks.onError = captureError;
    inputController.setCallbacks(inputCallbacks);
    TEST_ASSERT_TRUE(inputController.begin(2));
    TEST_ASSERT_FALSE(inputController.setGlobalPin(32, true));
    TEST_ASSERT_EQUAL_UINT32(0, inputTransport.sentCount);
    TEST_ASSERT_EQUAL_UINT32(1, inputCapture.errorCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ControllerError::UnsupportedCommand),
                            static_cast<uint8_t>(inputCapture.lastError));
}

void test_controller_dispatches_only_valid_addressed_frames() {
    FakeTransport transport;
    ControllerCapture capture;
    PihoController controller(transport);
    controller.setCallbacks(captureCallbacks(capture));
    TEST_ASSERT_TRUE(controller.begin(2));

    piho::CanFrame input{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::inputState(7, 0xA55A, input));
    transport.enqueue(input);

    piho::CanFrame otherDevice{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::setPin(3, 4, true, otherDevice));
    transport.enqueue(otherDevice);

    piho::CanFrame local{};
    TEST_ASSERT_TRUE(piho::ProtocolCodec::setPin(2, 5, true, local));
    transport.enqueue(local);

    piho::CanFrame malformed = local;
    malformed.length = 1;
    transport.enqueue(malformed);

    controller.poll();
    TEST_ASSERT_EQUAL_UINT32(1, transport.pollCalls);
    TEST_ASSERT_EQUAL_UINT32(1, capture.inputCalls);
    TEST_ASSERT_EQUAL_UINT8(7, capture.inputDevice);
    TEST_ASSERT_EQUAL_HEX16(0xA55A, capture.inputState);
    TEST_ASSERT_EQUAL_UINT32(1, capture.pinCalls);
    TEST_ASSERT_EQUAL_UINT8(5, capture.localPin);
    TEST_ASSERT_EQUAL_UINT32(1, capture.errorCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ControllerError::InvalidFrame),
                            static_cast<uint8_t>(capture.lastError));

    transport.sendResult = false;
    TEST_ASSERT_FALSE(controller.setGlobalPin(52, true));
    TEST_ASSERT_EQUAL_UINT32(2, capture.errorCalls);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ControllerError::Transport),
                            static_cast<uint8_t>(capture.lastError));
    controller.poll();
    TEST_ASSERT_EQUAL_UINT32(2, capture.errorCalls);
}
}  // namespace

void setUp() {}
void tearDown() {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_global_addresses_cover_every_device);
    RUN_TEST(test_set_pin_value_is_inside_payload);
    RUN_TEST(test_protocol_rejects_unrelated_and_malformed_frames);
    RUN_TEST(test_input_and_output_state_are_little_endian);
    RUN_TEST(test_trigger_router_only_toggles_on_rising_edges);
    RUN_TEST(test_debouncer_requires_a_stable_candidate);
    RUN_TEST(test_debouncer_handles_millisecond_wraparound);
    RUN_TEST(test_serial_frames_recover_from_noise_and_bad_checksum);
    RUN_TEST(test_trigger_storage_round_trips_and_rejects_corruption);
    RUN_TEST(test_controller_maps_global_outputs_and_avoids_self_echo);
    RUN_TEST(test_controller_dispatches_only_valid_addressed_frames);
    runGraphImageTests();
    runFlowEngineTests();
    runActionRuntimeTests();
    runGraphStoreTests();
    return UNITY_END();
}
