#include "piho/crc32.h"

namespace piho {

void Crc32::add(uint8_t byte) {
    value_ ^= byte;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        value_ = (value_ & 1u) != 0 ? (value_ >> 1) ^ 0xEDB88320u : value_ >> 1;
    }
}

void Crc32::add(const uint8_t *data, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        add(data[index]);
    }
}

}  // namespace piho
