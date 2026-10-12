#pragma once
#include "ArchiveJob.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <span>

namespace PKNPacker {
using EntryId = std::uint32_t;
struct ArchiveEntry {
    EntryId id = 0;
    std::u16string rawPath;
    std::string path; // Original path, represented as UTF-8.
    std::string lookupPath; // Loader comparison: slash and lowercase normalization.
    std::uint32_t flags = 0;
    std::uint32_t indexChecksum = 0;
    std::uint64_t offset = 0, size = 0, storedSize = 0;
    std::array<std::uint8_t, 16> cipherKey{}; // Often an MD5, but original optimized PNGs contain counterexamples.
};
struct ArchiveCatalog {
    std::filesystem::path source;
    std::string packageName;
    std::string logicalDirectory = "packs";
    std::uint8_t version = 2;
    std::uint64_t fileSize = 0, dataBase = 0, headerOffset = 0, indexOffset = 0;
    std::int64_t modifiedTime = 0;
    std::vector<ArchiveEntry> entries;
    const ArchiveEntry& Entry(EntryId id) const;
};
struct OpenOptions {
    std::string logicalDirectory = "packs";
    std::optional<std::string> packageName; // Intended identity when verifying an uncommitted temporary file.
};
ArchiveCatalog OpenArchive(const std::filesystem::path& path, const OpenOptions& options = {}, const ArchiveJob& job = {});
std::u16string FromUtf8(const std::string& text);
std::string ToUtf8(const std::u16string& text);
std::string NormalizePath(const std::string& text);
void ValidatePackageName(const std::string& name);
struct NamedResourcePath { std::string owner, path; };
struct ResourcePathConflict { std::string lookupPath; std::vector<std::string> owners; };
std::vector<ResourcePathConflict> FindResourcePathConflicts(std::span<const NamedResourcePath> paths, const ArchiveJob& job = {});
}
