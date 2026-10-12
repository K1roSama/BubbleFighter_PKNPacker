#pragma once
#include "ArchiveCatalog.h"
#include "ContentHash.h"
#include "FileIo.h"
#include "SnowCipher.h"
#include <functional>
#include <optional>

namespace PKNPacker {
using ByteSink = std::function<void(std::span<const std::uint8_t>)>;
ContentDigest ReadEntryTo(const ArchiveCatalog& archive, EntryId id, const ByteSink& sink,
                          const ArchiveJob& job = {}, std::optional<std::uint64_t> stopAfter = {});
std::vector<std::uint8_t> ReadEntry(const ArchiveCatalog& archive, EntryId id,
                                   std::uint64_t limit = 1024 * 1024, std::uint64_t offset = 0,
                                   const ArchiveJob& job = {});
ContentDigest ExportEntry(const ArchiveCatalog& archive, EntryId id, const std::filesystem::path& target,
                          bool overwrite = false, const ArchiveJob& job = {});
struct ValidationReport {
    std::uint64_t entries = 0, decodedBytes = 0, keyDigestMismatches = 0;
    std::string packageSha256;
};
ValidationReport ValidateArchive(const std::filesystem::path& archive, const OpenOptions& options = {},
                                 const ArchiveJob& job = {}, bool hashPackage = true);

namespace Detail {
struct NameParameters {
    std::uint64_t headerOffset = 0, indexOffset = 0;
    std::array<std::uint8_t, 128> headerKey{}, indexKey{};
};
NameParameters DeriveName(const std::string& logicalDirectory, const std::string& packageName);
std::array<std::uint8_t, 128> DeriveResourceKey(const std::u16string& uri, std::span<const std::uint8_t, 16> md5);
std::uint64_t Align(std::uint64_t value, std::uint64_t alignment);
void Pad(File& file, std::uint64_t end);
}
}
