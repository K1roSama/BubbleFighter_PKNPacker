#include "SnowCipher.h"
#include "SnowTables.h"
#include "ArchiveJob.h"

namespace PKNPacker {
std::uint32_t ReadU32(std::span<const std::uint8_t> b) {
    if (b.size() < 4) throw ArchiveError(ErrorCode::Bounds, "32 位字段被截断");
    return b[0] | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24);
}
void WriteU32(std::span<std::uint8_t> b, std::uint32_t value) {
    if (b.size() < 4) throw ArchiveError(ErrorCode::Bounds, "32 位输出字段不足");
    for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
SnowCipher::SnowCipher(std::span<const std::uint8_t> key) {
    if (key.size() < 16) throw ArchiveError(ErrorCode::Bounds, "SnowCipher 需要 16 字节密钥");
    for (unsigned i = 0; i < 4; ++i) {
        std::uint32_t word = 0;
        for (unsigned j = 0; j < 4; ++j) {
            // The original MOVSX sign-extends bytes before OR. This is intentional.
            const auto signedByte = key[i * 4 + j] < 128 ? std::int32_t(key[i * 4 + j]) : std::int32_t(key[i * 4 + j]) - 256;
            word = (word << 8) | static_cast<std::uint32_t>(signedByte);
        }
        state_[i] = state_[i + 8] = word;
        state_[i + 4] = state_[i + 12] = ~word;
    }
    for (unsigned i = 0; i < 32; ++i) Clock(true);
}
std::uint32_t SnowCipher::Clock(bool initialize) {
    const auto at = [&](unsigned relative) { return state_[(cursor_ + relative) & 15]; };
    std::uint32_t next = at(14) ^ (at(0) << 8) ^ SnowTables[5][at(0) >> 24]
                       ^ (at(5) >> 8) ^ SnowTables[4][at(5) & 255];
    if (initialize) next ^= (at(1) + first_) ^ second_;
    const auto first = at(11) + second_;
    const auto second = SnowTables[0][first_ >> 24] ^ SnowTables[1][(first_ >> 16) & 255]
                      ^ SnowTables[2][(first_ >> 8) & 255] ^ SnowTables[3][first_ & 255];
    const auto word = (next + first) ^ second ^ at(15);
    state_[cursor_] = next;
    first_ = first;
    second_ = second;
    cursor_ = (cursor_ + 15) & 15;
    return word;
}
std::uint32_t SnowCipher::NextWord() { return Clock(false); }
void SnowCipher::Transform(std::span<std::uint8_t> bytes, bool encode) {
    if (bytes.size() % 4) throw ArchiveError(ErrorCode::InvalidFormat, "密码块长度必须为 4 的倍数");
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const auto word = ReadU32(bytes.subspan(i, 4)), mask = NextWord();
        WriteU32(bytes.subspan(i, 4), encode ? word + mask : word - mask);
    }
}
}
