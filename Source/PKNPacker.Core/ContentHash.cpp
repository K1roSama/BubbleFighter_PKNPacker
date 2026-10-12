#include "ContentHash.h"
#include "FileIo.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>

namespace PKNPacker {
namespace {
void CheckStatus(NTSTATUS status) {
    if (status < 0) throw ArchiveError(ErrorCode::Io, "BCrypt 哈希失败：" + std::to_string(status));
}
struct HashProviders {
    BCRYPT_ALG_HANDLE md5 = nullptr, sha256 = nullptr;
    HashProviders() {
        CheckStatus(BCryptOpenAlgorithmProvider(&md5, BCRYPT_MD5_ALGORITHM, nullptr, 0));
        CheckStatus(BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
    }
    ~HashProviders() { BCryptCloseAlgorithmProvider(md5, 0); BCryptCloseAlgorithmProvider(sha256, 0); }
};
}
ContentHash::ContentHash(HashAlgorithm algorithm) {
    static HashProviders providers;
    const auto provider = algorithm == HashAlgorithm::Md5 ? providers.md5 : providers.sha256;
    DWORD length = 0, actual = 0;
    CheckStatus(BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &actual, 0));
    object_.resize(length);
    digestSize_ = algorithm == HashAlgorithm::Md5 ? 16 : 32;
    BCRYPT_HASH_HANDLE handle = nullptr;
    CheckStatus(BCryptCreateHash(provider, &handle, object_.data(), length, nullptr, 0, 0));
    hash_ = handle;
}
ContentHash::~ContentHash() { if (hash_) BCryptDestroyHash(hash_); }
void ContentHash::Update(std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto amount = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        CheckStatus(BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), amount, 0));
        bytes = bytes.subspan(amount);
    }
}
std::vector<std::uint8_t> ContentHash::Finish() {
    std::vector<std::uint8_t> digest(digestSize_);
    CheckStatus(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0));
    return digest;
}
std::string Hex(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const auto value : bytes) { text.push_back(digits[value >> 4]); text.push_back(digits[value & 15]); }
    return text;
}
std::vector<std::uint8_t> Unhex(const std::string& text) {
    if (text.size() % 2) throw ArchiveError(ErrorCode::InvalidFormat, "十六进制长度错误");
    const auto digit = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a') + 10;
        if (c >= 'A' && c <= 'F') return unsigned(c - 'A') + 10;
        throw ArchiveError(ErrorCode::InvalidFormat, "非法十六进制字符");
    };
    std::vector<std::uint8_t> bytes(text.size() / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(digit(text[i * 2]) * 16 + digit(text[i * 2 + 1]));
    return bytes;
}
ContentDigest HashFile(const std::filesystem::path& path, const ArchiveJob& job) {
    Detail::File file(path);
    ContentHash md5(HashAlgorithm::Md5), sha(HashAlgorithm::Sha256);
    ContentDigest result;
    result.size = file.Size();
    std::array<std::uint8_t, 65536> buffer{};
    for (std::uint64_t done = 0; done < result.size;) {
        job.Check();
        auto chunk = std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), result.size - done)));
        file.Read(chunk);
        md5.Update(chunk); sha.Update(chunk);
        done += chunk.size();
    }
    const auto digest = md5.Finish();
    std::copy(digest.begin(), digest.end(), result.md5.begin());
    result.sha256 = Hex(sha.Finish());
    return result;
}
}
