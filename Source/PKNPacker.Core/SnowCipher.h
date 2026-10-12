#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace PKNPacker {
class SnowCipher {
public:
    explicit SnowCipher(std::span<const std::uint8_t> key);
    void Transform(std::span<std::uint8_t> bytes, bool encode = false);
    std::uint32_t NextWord();
private:
    std::uint32_t Clock(bool initialize);
    std::array<std::uint32_t, 16> state_{};
    std::uint32_t first_ = 0, second_ = 0;
    unsigned cursor_ = 15;
};
std::uint32_t ReadU32(std::span<const std::uint8_t> bytes);
void WriteU32(std::span<std::uint8_t> bytes, std::uint32_t value);
}
