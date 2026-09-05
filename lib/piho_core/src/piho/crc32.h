#pragma once

#include <cstddef>
#include <cstdint>

namespace piho {

class Crc32 {
   public:
    void add(uint8_t byte);
    void add(const uint8_t *data, std::size_t size);
    uint32_t value() const { return ~value_; }

   private:
    uint32_t value_ = 0xFFFFFFFFu;
};

}  // namespace piho
