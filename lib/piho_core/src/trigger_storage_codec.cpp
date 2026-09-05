#include "piho/trigger_storage_codec.h"

#include "piho/crc32.h"

namespace piho {
namespace {

constexpr uint8_t kMagic[] = {'P', 'H', 'T', 'R'};
constexpr uint8_t kFormatVersion = 1;
constexpr uint8_t kRecordSize = 3;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kChecksumSize = 4;

void encodeUint16(uint16_t value, uint8_t *output) {
    output[0] = static_cast<uint8_t>(value & 0xFFu);
    output[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

uint16_t decodeUint16(const uint8_t *input) {
    return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8);
}

void encodeUint32(uint32_t value, uint8_t *output) {
    for (uint8_t byte = 0; byte < 4; ++byte) {
        output[byte] = static_cast<uint8_t>((value >> (byte * 8)) & 0xFFu);
    }
}

uint32_t decodeUint32(const uint8_t *input) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(input[byte]) << (byte * 8);
    }
    return value;
}

}  // namespace

TriggerStorageError TriggerStorageCodec::encode(const TriggerTable &rules, uint32_t generation, uint8_t *output,
                                                std::size_t outputCapacity, std::size_t &outputSize) {
    outputSize = 0;
    const std::size_t required = kHeaderSize + rules.size() * kRecordSize + kChecksumSize;
    if (output == nullptr || outputCapacity < required) {
        return TriggerStorageError::BufferTooSmall;
    }

    for (std::size_t index = 0; index < sizeof(kMagic); ++index) {
        output[index] = kMagic[index];
    }
    output[4] = kFormatVersion;
    output[5] = kRecordSize;
    encodeUint16(static_cast<uint16_t>(rules.size()), &output[6]);
    encodeUint32(generation, &output[8]);

    std::size_t position = kHeaderSize;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const TriggerRule &rule = rules.at(index);
        output[position++] = rule.inputDevice;
        output[position++] = rule.inputPin;
        output[position++] = rule.outputPin;
    }

    Crc32 checksum;
    checksum.add(output, position);
    encodeUint32(checksum.value(), &output[position]);
    outputSize = position + kChecksumSize;
    return TriggerStorageError::None;
}

TriggerStorageError TriggerStorageCodec::decode(const uint8_t *data, std::size_t size, TriggerTable &rules,
                                                uint32_t &generation) {
    if (data == nullptr || size < kHeaderSize + kChecksumSize) {
        return TriggerStorageError::InvalidLength;
    }
    for (std::size_t index = 0; index < sizeof(kMagic); ++index) {
        if (data[index] != kMagic[index]) {
            return TriggerStorageError::InvalidMagic;
        }
    }
    if (data[4] != kFormatVersion || data[5] != kRecordSize) {
        return TriggerStorageError::UnsupportedVersion;
    }

    const uint16_t count = decodeUint16(&data[6]);
    if (count > TriggerTable::kCapacity) {
        return TriggerStorageError::InvalidLength;
    }
    const std::size_t expected = kHeaderSize + static_cast<std::size_t>(count) * kRecordSize + kChecksumSize;
    if (size != expected) {
        return TriggerStorageError::InvalidLength;
    }

    Crc32 checksum;
    checksum.add(data, size - kChecksumSize);
    if (checksum.value() != decodeUint32(&data[size - kChecksumSize])) {
        return TriggerStorageError::InvalidChecksum;
    }

    TriggerTable candidate;
    std::size_t position = kHeaderSize;
    for (uint16_t index = 0; index < count; ++index) {
        const TriggerRule rule{data[position], data[position + 1], data[position + 2]};
        position += kRecordSize;
        if (candidate.upsert(rule) != TriggerUpdateResult::Inserted) {
            return TriggerStorageError::InvalidRule;
        }
    }

    rules = candidate;
    generation = decodeUint32(&data[8]);
    return TriggerStorageError::None;
}

}  // namespace piho
