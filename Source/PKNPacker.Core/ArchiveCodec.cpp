#include "ArchiveCodec.h"
#include <zlib.h>
#include <algorithm>
#include <limits>

namespace PKNPacker {
namespace Detail {
std::uint64_t Align(std::uint64_t value, std::uint64_t alignment) {
    if (!alignment || value > UINT64_MAX - alignment + 1) throw ArchiveError(ErrorCode::Bounds, "对齐计算溢出");
    return (value + alignment - 1) / alignment * alignment;
}
void Pad(File& file, std::uint64_t end) {
    if (file.Tell() > end) throw ArchiveError(ErrorCode::Bounds, "写入位置超出预留范围");
    std::array<std::uint8_t, 65536> zeros{};
    while (file.Tell() < end) file.Write(std::span(zeros).first(static_cast<std::size_t>(std::min<std::uint64_t>(end - file.Tell(), zeros.size()))));
}
NameParameters DeriveName(const std::string& directory, const std::string& package) {
    const auto name = FromUtf8(NormalizePath(directory + "/" + package));
    std::uint32_t sum = 0;
    for (const auto unit : name) sum += unit;
    NameParameters parameters;
    parameters.headerOffset = sum % 312 + 30;
    parameters.indexOffset = (3 * sum) % 212 + 42 + parameters.headerOffset;
    for (std::size_t i = 0; i < 128; ++i) {
        parameters.headerKey[i] = static_cast<std::uint8_t>(i + (name[i % name.size()] & 255));
        parameters.indexKey[i] = static_cast<std::uint8_t>(i + (i % 3 + 2) * (name[name.size() - 1 - i % name.size()] & 255));
    }
    return parameters;
}
std::array<std::uint8_t, 128> DeriveResourceKey(const std::u16string& uri, std::span<const std::uint8_t, 16> md5) {
    if (uri.empty()) throw ArchiveError(ErrorCode::InvalidFormat, "空资源路径无法派生密钥");
    std::array<std::uint8_t, 128> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(i + (uri[i % uri.size()] & 255) * (i % 5 + md5[i % 16] + 2));
    return key;
}
}
ContentDigest ReadEntryTo(const ArchiveCatalog& archive, EntryId id, const ByteSink& sink,
                          const ArchiveJob& job, std::optional<std::uint64_t> stopAfter) {
    const auto& entry = archive.Entry(id);
    Detail::File file(archive.source);
    if (file.Size() != archive.fileSize || Detail::ModifiedTime(archive.source) != archive.modifiedTime)
        throw ArchiveError(ErrorCode::Integrity, "原包在打开后发生变化，请重新打开");
    file.Seek(entry.offset);
    SnowCipher cipher(Detail::DeriveResourceKey(entry.rawPath, entry.cipherKey));
    ContentHash md5(HashAlgorithm::Md5), sha(HashAlgorithm::Sha256);
    std::array<std::uint8_t, 65536> input{}, output{};
    ContentDigest digest;
    if (!entry.size && !entry.storedSize) {
        const auto hash = md5.Finish(); std::copy(hash.begin(), hash.end(), digest.md5.begin());
        digest.sha256 = Hex(sha.Finish()); return digest;
    }
    std::uint64_t read = 0, decoded = 0;
    z_stream stream{};
    struct InflateGuard { z_stream* stream; ~InflateGuard() { if (stream) inflateEnd(stream); } } guard{nullptr};
    bool ended = false;
    if (entry.flags & 1) {
        if (inflateInit(&stream) != Z_OK) throw ArchiveError(ErrorCode::Io, "无法初始化 zlib");
        guard.stream = &stream;
    }
    const auto consume = [&](std::span<const std::uint8_t> bytes) {
        if (bytes.size() > entry.size - decoded) throw ArchiveError(ErrorCode::Bounds, "解压内容超出声明大小：" + entry.path);
        auto emit = bytes;
        if (stopAfter && *stopAfter < decoded + emit.size()) emit = emit.first(static_cast<std::size_t>(*stopAfter > decoded ? *stopAfter - decoded : 0));
        if (!emit.empty()) { if (sink) sink(emit); md5.Update(emit); sha.Update(emit); }
        decoded += bytes.size();
    };
    while (read < entry.storedSize) {
        job.Check();
        auto chunk = std::span(input).first(static_cast<std::size_t>(std::min<std::uint64_t>(input.size(), entry.storedSize - read)));
        file.Read(chunk);
        if (entry.flags & 2) cipher.Transform(chunk);
        else if ((entry.flags & 4) && read < 1024)
            cipher.Transform(chunk.first(static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), 1024 - read))));
        read += chunk.size();
        if (entry.flags & 1) {
            stream.next_in = chunk.data(); stream.avail_in = static_cast<uInt>(chunk.size());
            do {
                stream.next_out = output.data(); stream.avail_out = static_cast<uInt>(output.size());
                const auto before = stream.avail_in;
                const auto status = inflate(&stream, Z_NO_FLUSH);
                const auto amount = output.size() - stream.avail_out;
                consume(std::span(output).first(amount));
                if (stopAfter && *stopAfter < entry.size && decoded >= *stopAfter) return digest;
                if (status == Z_STREAM_END) { ended = true; break; }
                if (status != Z_OK && status != Z_BUF_ERROR) throw ArchiveError(ErrorCode::Integrity, "zlib 解码或校验失败：" + entry.path);
                if (!amount && before == stream.avail_in) throw ArchiveError(ErrorCode::InvalidFormat, "zlib 解码未推进：" + entry.path);
            } while (stream.avail_in || !stream.avail_out);
            if (ended) {
                const auto extra = entry.storedSize - read + stream.avail_in;
                if (extra > ((entry.flags & 2) ? 3U : 0U)) throw ArchiveError(ErrorCode::InvalidFormat, "压缩流含多余内容：" + entry.path);
                break;
            }
        } else {
            consume(chunk.first(static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), entry.size - decoded))));
            if (stopAfter && *stopAfter < entry.size && decoded >= *stopAfter) return digest;
        }
    }
    if (decoded != entry.size || ((entry.flags & 1) && !ended)) throw ArchiveError(ErrorCode::Integrity, "正文长度或压缩终点错误：" + entry.path);
    const auto hash = md5.Finish();
    std::copy(hash.begin(), hash.end(), digest.md5.begin());
    // The original loader uses the 16-byte index field only for key derivation.
    // Valid original PNGs can have a different content MD5 (see PknFormat.md).
    digest.sha256 = Hex(sha.Finish()); digest.size = decoded;
    return digest;
}
std::vector<std::uint8_t> ReadEntry(const ArchiveCatalog& archive, EntryId id, std::uint64_t limit,
                                   std::uint64_t offset, const ArchiveJob& job) {
    const auto& entry = archive.Entry(id);
    if (offset > entry.size || limit > 1024 * 1024) throw ArchiveError(ErrorCode::Bounds, "预览每页最多 1 MiB，偏移须位于文件内");
    const auto amount = std::min(limit, entry.size - offset);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(amount));
    std::uint64_t done = 0;
    ReadEntryTo(archive, id, [&](std::span<const std::uint8_t> chunk) {
        if (done + chunk.size() > offset && done < offset + amount) {
            const auto start = offset > done ? static_cast<std::size_t>(offset - done) : 0;
            const auto end = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), offset + amount - done));
            bytes.insert(bytes.end(), chunk.begin() + start, chunk.begin() + end);
        }
        done += chunk.size();
    }, job, offset + amount);
    return bytes;
}
ContentDigest ExportEntry(const ArchiveCatalog& archive, EntryId id, const std::filesystem::path& target,
                          bool overwrite, const ArchiveJob& job) {
    if (Detail::SamePath(target, archive.source))
        throw ArchiveError(ErrorCode::Conflict, "导出不能覆盖输入资源包");
    Detail::AtomicFile output(target, overwrite);
    Detail::File file(output.temporary, true);
    const auto digest = ReadEntryTo(archive, id, [&](auto bytes) { file.Write(bytes); }, job);
    job.Check(); file.Flush(); file.Close(); output.Commit();
    return digest;
}
ValidationReport ValidateArchive(const std::filesystem::path& path, const OpenOptions& options,
                                 const ArchiveJob& job, bool hashPackage) {
    const auto archive = OpenArchive(path, options, job);
    ValidationReport report;
    for (const auto& entry : archive.entries) {
        job.Report("验证正文", archive.packageName + ": " + entry.path, report.entries, archive.entries.size());
        if (ReadEntryTo(archive, entry.id, {}, job).md5 != entry.cipherKey) ++report.keyDigestMismatches;
        ++report.entries; report.decodedBytes += entry.size;
    }
    if (hashPackage) report.packageSha256 = HashFile(path, job).sha256;
    return report;
}
}
