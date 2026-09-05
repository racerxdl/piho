#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace piho {

constexpr std::size_t kCanPayloadCapacity = 8;
constexpr uint32_t kCanIdentifierMask = 0x1FFFFFFFu;

struct CanFrame {
    uint32_t identifier = 0;
    uint8_t length = 0;
    bool extended = false;
    bool remote = false;
    uint8_t data[kCanPayloadCapacity]{};
};

static_assert(std::is_trivially_copyable<CanFrame>::value, "CAN frames must remain queue-safe values");

}  // namespace piho
