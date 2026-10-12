#pragma once
#include "ArchiveJob.h"
#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace PKNPacker {
enum class HashAlgorithm { Md5, Sha256 };
class ContentHash {
public:
    explicit ContentHash(HashAlgorithm algorithm);
    ~ContentHash();
    ContentHash(const ContentHash&) = delete;
    ContentHash& operator=(const ContentHash&) = delete;
    void Update(std::span<const std::uint8_t> bytes);
    std::vector<std::uint8_t> Finish();
private:
    void* hash_ = nullptr;
    std::vector<std::uint8_t> object_;
    std::size_t digestSize_ = 0;
};
struct ContentDigest { std::array<std::uint8_t, 16> md5{}; std::string sha256; std::uint64_t size = 0; };
std::string Hex(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> Unhex(const std::string& text);
ContentDigest HashFile(const std::filesystem::path& file, const ArchiveJob& job = {});
}
