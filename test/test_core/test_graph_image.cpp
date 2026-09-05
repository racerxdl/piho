#include <unity.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "piho/graph_image.h"

namespace {

constexpr const char *kGoldenPath = "tools/piho-flow/test/fixtures/synthetic.phg";

std::vector<uint8_t> loadGoldenImage() {
    std::ifstream input(kGoldenPath, std::ios::binary);
    TEST_ASSERT_TRUE_MESSAGE(input.good(), "cannot open shared synthetic.phg fixture");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>());
}

void writeUint16(std::vector<uint8_t> &image, std::size_t offset, uint16_t value) {
    image[offset] = static_cast<uint8_t>(value & 0xFFu);
    image[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void writeUint32(std::vector<uint8_t> &image, std::size_t offset, uint32_t value) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        image[offset + byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

void resign(std::vector<uint8_t> &image) {
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::checksum(
            piho::GraphImageSource::fromMemory(image.data(), image.size()), checksum)));
    writeUint32(image, piho::kGraphChecksumOffset, checksum);
}

piho::GraphImageError validate(const std::vector<uint8_t> &image, piho::GraphManifest &manifest) {
    return piho::GraphImageCodec::validate(
        piho::GraphImageSource::fromMemory(image.data(), image.size()), manifest);
}

void test_graph_codec_decodes_shared_golden_image() {
    const std::vector<uint8_t> image = loadGoldenImage();
    TEST_ASSERT_EQUAL_UINT32(244, image.size());

    const piho::GraphImageSource source =
        piho::GraphImageSource::fromMemory(image.data(), image.size());
    uint32_t checksum = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::None),
                            static_cast<uint8_t>(piho::GraphImageCodec::checksum(source, checksum)));
    TEST_ASSERT_EQUAL_HEX32(0xAE85369A, checksum);

    piho::GraphManifest manifest{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::None),
                            static_cast<uint8_t>(piho::GraphImageCodec::validate(source, manifest)));
    TEST_ASSERT_EQUAL_UINT16(1, manifest.format);
    TEST_ASSERT_EQUAL_UINT16(1, manifest.executorApi);
    TEST_ASSERT_EQUAL_UINT32(1, manifest.generation);
    TEST_ASSERT_EQUAL_UINT32(image.size(), manifest.imageSize);
    TEST_ASSERT_EQUAL_HEX32(0xAE85369A, manifest.checksum);
    TEST_ASSERT_EQUAL_UINT16(4, manifest.deviceCount);
    TEST_ASSERT_EQUAL_UINT16(2, manifest.inputCount);
    TEST_ASSERT_EQUAL_UINT16(3, manifest.flowCount);
    TEST_ASSERT_EQUAL_UINT16(3, manifest.routeCount);
    TEST_ASSERT_EQUAL_UINT16(4, manifest.actionReferenceCount);
    TEST_ASSERT_EQUAL_UINT16(4, manifest.actionCount);
    TEST_ASSERT_EQUAL_UINT32(56, manifest.deviceOffset);
    TEST_ASSERT_EQUAL_UINT32(120, manifest.inputOffset);
    TEST_ASSERT_EQUAL_UINT32(136, manifest.routeOffset);
    TEST_ASSERT_EQUAL_UINT32(172, manifest.actionReferenceOffset);
    TEST_ASSERT_EQUAL_UINT32(180, manifest.actionOffset);

    const piho::GraphDeviceRecord *inputDevice = manifest.findDevice(1);
    TEST_ASSERT_NOT_NULL(inputDevice);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphDeviceRole::Input),
                            static_cast<uint8_t>(inputDevice->role));
    TEST_ASSERT_EQUAL_UINT16(1, inputDevice->inputCount);
    TEST_ASSERT_EQUAL_UINT16(2, inputDevice->routeCount);

    piho::LocalInputGraph localInput{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::loadInputSection(source, manifest, 1, localInput)));
    TEST_ASSERT_EQUAL_UINT32(1, localInput.identity.generation);
    TEST_ASSERT_EQUAL_HEX32(0xAE85369A, localInput.identity.checksum);
    TEST_ASSERT_EQUAL_UINT8(1, localInput.device);
    TEST_ASSERT_EQUAL_UINT16(1, localInput.inputCount);
    TEST_ASSERT_EQUAL_UINT16(2, localInput.routeCount);
    TEST_ASSERT_EQUAL_UINT16(3, localInput.actionReferenceCount);
    TEST_ASSERT_EQUAL_UINT16(3, localInput.referencedActionCount);
    TEST_ASSERT_EQUAL_UINT16(1, localInput.inputs[0].id);
    TEST_ASSERT_EQUAL_UINT8(2, localInput.inputs[0].pin);
    TEST_ASSERT_EQUAL_UINT16(25, localInput.inputs[0].debounceMs);
    TEST_ASSERT_EQUAL_UINT16(2, localInput.routes[0].id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphEdge::Rising),
                            static_cast<uint8_t>(localInput.routes[0].edge));
    TEST_ASSERT_EQUAL_UINT16(1, localInput.routes[1].id);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphEdge::Falling),
                            static_cast<uint8_t>(localInput.routes[1].edge));
    TEST_ASSERT_EQUAL_UINT16(1, localInput.actionReferences[0]);
    TEST_ASSERT_EQUAL_UINT16(4, localInput.actionReferences[1]);
    TEST_ASSERT_EQUAL_UINT16(3, localInput.actionReferences[2]);

    const piho::GraphActionRecord *delayed = localInput.findAction(1);
    TEST_ASSERT_NOT_NULL(delayed);
    TEST_ASSERT_EQUAL_UINT8(8, delayed->targetDevice);
    TEST_ASSERT_EQUAL_UINT8(4, delayed->targetPin);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphOperation::Set),
                            static_cast<uint8_t>(delayed->operation));
    TEST_ASSERT_TRUE(delayed->value);
    TEST_ASSERT_EQUAL_UINT32(60000, delayed->delayMs);

    const piho::GraphActionRecord *toggle = localInput.findAction(4);
    TEST_ASSERT_NOT_NULL(toggle);
    TEST_ASSERT_EQUAL_UINT8(7, toggle->targetDevice);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphOperation::Toggle),
                            static_cast<uint8_t>(toggle->operation));
    const piho::GraphActionRecord *pulse = localInput.findAction(3);
    TEST_ASSERT_NOT_NULL(pulse);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphOperation::Pulse),
                            static_cast<uint8_t>(pulse->operation));
    TEST_ASSERT_EQUAL_UINT32(250, pulse->delayMs);
    TEST_ASSERT_EQUAL_UINT32(1000, pulse->durationMs);
    TEST_ASSERT_NULL(localInput.findAction(2));

    piho::LocalOutputGraph localOutput{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::loadOutputSection(source, manifest, 7, localOutput)));
    TEST_ASSERT_EQUAL_UINT8(7, localOutput.device);
    TEST_ASSERT_EQUAL_UINT16(3, localOutput.actionCount);
    TEST_ASSERT_NOT_NULL(localOutput.findAction(2));
    TEST_ASSERT_NOT_NULL(localOutput.findAction(3));
    TEST_ASSERT_NOT_NULL(localOutput.findAction(4));
    TEST_ASSERT_NULL(localOutput.findAction(1));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphOperation::CopySource),
                            static_cast<uint8_t>(localOutput.findAction(2)->operation));

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::DeviceRoleMismatch),
        static_cast<uint8_t>(piho::GraphImageCodec::loadOutputSection(source, manifest, 1, localOutput)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::DeviceNotFound),
        static_cast<uint8_t>(piho::GraphImageCodec::loadInputSection(source, manifest, 31, localInput)));
}

void test_graph_codec_rejects_header_offset_and_checksum_corruption() {
    const std::vector<uint8_t> golden = loadGoldenImage();
    piho::GraphManifest manifest{};

    std::vector<uint8_t> changed = golden;
    changed[0] = 'X';
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidMagic),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint16(changed, 4, 2);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::UnsupportedFormat),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint16(changed, 6, 2);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::UnsupportedExecutorApi),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint32(changed, 12, 243);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidLength),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint32(changed, 32, 57);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidOffsets),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    changed[80] ^= 0x01;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidChecksum),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed.assign(golden.begin(), golden.end() - 1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidLength),
                            static_cast<uint8_t>(validate(changed, manifest)));
}

void test_graph_codec_rejects_invalid_counts_sections_and_records() {
    const std::vector<uint8_t> golden = loadGoldenImage();
    piho::GraphManifest manifest{};
    std::vector<uint8_t> changed = golden;

    writeUint16(changed, 16, 33);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidCount),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    changed[57] = 3;
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidDevice),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint16(changed, 60, 0);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidSection),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint16(changed, 128, 1);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::DuplicateRecord),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    changed[123] = 16;
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidInput),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    changed[145] = 0;
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidRoute),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    writeUint16(changed, 172, 0);
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidReference),
                            static_cast<uint8_t>(validate(changed, manifest)));

    changed = golden;
    changed[184] = 9;
    resign(changed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::InvalidAction),
                            static_cast<uint8_t>(validate(changed, manifest)));
}

struct FailingReader {
    const uint8_t *data = nullptr;
    std::size_t readableBytes = 0;
};

bool readUntilLimit(const void *context, std::size_t offset, uint8_t *output, std::size_t length) {
    const auto &reader = *static_cast<const FailingReader *>(context);
    if (offset > reader.readableBytes || length > reader.readableBytes - offset) {
        return false;
    }
    std::memcpy(output, reader.data + offset, length);
    return true;
}

struct BoundedReader {
    const uint8_t *data = nullptr;
    std::size_t size = 0;
    mutable std::size_t largestRead = 0;
};

bool readBounded(const void *context, std::size_t offset, uint8_t *output, std::size_t length) {
    const auto &reader = *static_cast<const BoundedReader *>(context);
    if (length > 64 || offset > reader.size || length > reader.size - offset) {
        return false;
    }
    if (length > reader.largestRead) {
        reader.largestRead = length;
    }
    std::memcpy(output, reader.data + offset, length);
    return true;
}

void test_graph_codec_is_bounded_and_preserves_outputs_on_failure() {
    const std::vector<uint8_t> golden = loadGoldenImage();
    FailingReader reader{golden.data(), 100};
    const piho::GraphImageSource source{&reader, golden.size(), readUntilLimit};

    piho::GraphManifest active{};
    active.generation = 77;
    active.checksum = 0x12345678;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(piho::GraphImageError::ReadFailure),
                            static_cast<uint8_t>(piho::GraphImageCodec::validate(source, active)));
    TEST_ASSERT_EQUAL_UINT32(77, active.generation);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, active.checksum);

    piho::LocalInputGraph localActive{};
    localActive.identity.generation = 88;
    localActive.device = 9;
    piho::GraphManifest validManifest{};
    const piho::GraphImageSource validSource =
        piho::GraphImageSource::fromMemory(golden.data(), golden.size());
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::validate(validSource, validManifest)));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::ReadFailure),
        static_cast<uint8_t>(
            piho::GraphImageCodec::loadInputSection(source, validManifest, 1, localActive)));
    TEST_ASSERT_EQUAL_UINT32(88, localActive.identity.generation);
    TEST_ASSERT_EQUAL_UINT8(9, localActive.device);

    BoundedReader boundedReader{golden.data(), golden.size(), 0};
    piho::GraphManifest boundedManifest{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::None),
        static_cast<uint8_t>(piho::GraphImageCodec::validate(
            piho::GraphImageSource{&boundedReader, golden.size(), readBounded}, boundedManifest)));
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(64, boundedReader.largestRead);

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(piho::GraphImageError::ReadFailure),
        static_cast<uint8_t>(piho::GraphImageCodec::validate(
            piho::GraphImageSource::fromMemory(nullptr, golden.size()), active)));
}

}  // namespace

void runGraphImageTests() {
    RUN_TEST(test_graph_codec_decodes_shared_golden_image);
    RUN_TEST(test_graph_codec_rejects_header_offset_and_checksum_corruption);
    RUN_TEST(test_graph_codec_rejects_invalid_counts_sections_and_records);
    RUN_TEST(test_graph_codec_is_bounded_and_preserves_outputs_on_failure);
}
