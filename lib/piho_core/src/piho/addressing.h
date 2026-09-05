#pragma once

#include <cstdint>

namespace piho {

constexpr uint8_t kDeviceCount = 32;
constexpr uint8_t kPinsPerDevice = 16;
constexpr uint8_t kBytesPerDevice = kPinsPerDevice / 8;
constexpr uint16_t kGlobalPinCount = static_cast<uint16_t>(kDeviceCount) * kPinsPerDevice;
constexpr uint16_t kGlobalByteCount = static_cast<uint16_t>(kDeviceCount) * kBytesPerDevice;

struct PinAddress {
    uint8_t device = 0;
    uint8_t localPin = 0;
};

struct ByteAddress {
    uint8_t device = 0;
    uint8_t localByte = 0;
};

constexpr bool isPhysicalDevice(uint8_t device) {
    return device < kDeviceCount;
}

constexpr bool decodeGlobalPin(uint16_t globalPin, PinAddress &address) {
    if (globalPin >= kGlobalPinCount) {
        return false;
    }
    address.device = static_cast<uint8_t>(globalPin / kPinsPerDevice);
    address.localPin = static_cast<uint8_t>(globalPin % kPinsPerDevice);
    return true;
}

constexpr bool decodeGlobalByte(uint16_t globalByte, ByteAddress &address) {
    if (globalByte >= kGlobalByteCount) {
        return false;
    }
    address.device = static_cast<uint8_t>(globalByte / kBytesPerDevice);
    address.localByte = static_cast<uint8_t>(globalByte % kBytesPerDevice);
    return true;
}

constexpr uint16_t globalPin(uint8_t device, uint8_t localPin) {
    return static_cast<uint16_t>(device) * kPinsPerDevice + localPin;
}

constexpr uint16_t globalByte(uint8_t device, uint8_t localByte) {
    return static_cast<uint16_t>(device) * kBytesPerDevice + localByte;
}

}  // namespace piho
