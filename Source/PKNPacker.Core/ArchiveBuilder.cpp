#include "ArchiveWorkspace.h"
#include <zlib.h>
#include <algorithm>
#include <unordered_map>

namespace PKNPacker {
namespace {
void EncodeInput(const std::filesystem::path& source, const ArchiveEntry& entry, Detail::File& output, const ArchiveJob& job) {
    Detail::File input(source);
    if (input.Size() != entry.size) throw ArchiveError(ErrorCode::Integrity, "输入文件在检查后发生变化");
    SnowCipher cipher(Detail::DeriveResourceKey(entry.rawPath, entry.cipherKey));
    std::uint64_t written = 0;
    std::vector<std::uint8_t> pending;
    pending.reserve(65540);
    const auto emit = [&](std::span<const std::uint8_t> bytes, bool finish) {
        pending.insert(pending.end(), bytes.begin(), bytes.end());
        if (finish && ((entry.flags & 2) || ((entry.flags & 4) && written + pending.size() < 1024)))
            pending.resize(static_cast<std::size_t>(Detail::Align(pending.size(), 4)), 0);
        const auto count = finish ? pending.size() : pending.size() / 4 * 4;
        auto block = std::span(pending).first(count);
        if (entry.flags & 2) cipher.Transform(block, true);
        else if ((entry.flags & 4) && written < 1024)
            cipher.Transform(block.first(static_cast<std::size_t>(std::min<std::uint64_t>(block.size(), 1024 - written))), true);
        output.Write(block);
        written += block.size();
        pending.erase(pending.begin(), pending.begin() + count);
    };
    std::array<std::uint8_t, 65536> buffer{}, compressed{};
    z_stream stream{};
    struct DeflateGuard { z_stream* stream; ~DeflateGuard() { if (stream) deflateEnd(stream); } } guard{nullptr};
    if (entry.flags & 1) {
        if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) throw ArchiveError(ErrorCode::Io, "无法初始化 zlib 压缩器");
        guard.stream = &stream;
    }
    std::uint64_t read = 0;
    do {
        job.Check();
        auto chunk = std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), entry.size - read)));
        input.Read(chunk); read += chunk.size();
        if (entry.flags & 1) {
            stream.next_in = chunk.data(); stream.avail_in = static_cast<uInt>(chunk.size());
            const auto flush = read == entry.size ? Z_FINISH : Z_NO_FLUSH;
            int status = Z_OK;
            do {
                stream.next_out = compressed.data(); stream.avail_out = static_cast<uInt>(compressed.size());
                status = deflate(&stream, flush);
                if (status != Z_OK && status != Z_STREAM_END) throw ArchiveError(ErrorCode::Io, "zlib 压缩失败");
                emit(std::span(compressed).first(compressed.size() - stream.avail_out), false);
            } while (stream.avail_in || !stream.avail_out || (flush == Z_FINISH && status != Z_STREAM_END));
        } else emit(chunk, false);
    } while (read < entry.size);
    emit({}, true);
}
}
ValidationReport BuildArchive(const ArchiveWorkspace& workspace, const std::filesystem::path& destination,
                              const BuildOptions& options, const ArchiveJob& job) {
    job.Check();
    const auto target = std::filesystem::absolute(destination).lexically_normal();
    const auto packageName = options.packageName.value_or(Detail::PathUtf8(target.filename()));
    ValidatePackageName(packageName);
    if (packageName != Detail::PathUtf8(target.filename())) throw ArchiveError(ErrorCode::Conflict, "输出文件名必须与生成时的包身份一致");
    if (!workspace.manifest.source.empty() && Detail::SamePath(target, workspace.manifest.source))
        throw ArchiveError(ErrorCode::Conflict, "封包输出不能覆盖输入原包");
    const auto changeSummary = workspace.CheckChanges(job);
    if (!changeSummary.Valid()) throw ArchiveError(ErrorCode::Conflict, changeSummary.conflicts.front());
    std::optional<ArchiveCatalog> original;
    if (!workspace.manifest.source.empty()) {
        job.Report("核验原包身份", workspace.manifest.originalPackageName, 0, 1);
        if (HashFile(workspace.manifest.source, job).sha256 != workspace.manifest.sourceSha256)
            throw ArchiveError(ErrorCode::Integrity, "原包 SHA-256 与工作区清单不一致");
        original = OpenArchive(workspace.manifest.source, {workspace.manifest.logicalDirectory, workspace.manifest.originalPackageName}, job);
        std::vector<bool> present(original->entries.size(), false);
        for (const auto& entry : workspace.manifest.entries) if (entry.change != ChangeKind::Add) {
            if (entry.id >= present.size() || present[entry.id] || entry.path != original->Entry(entry.id).path)
                throw ArchiveError(ErrorCode::Integrity, "清单中的原始条目身份被修改");
            if (entry.original) {
                const auto& actual = original->Entry(entry.id); const auto& recorded = *entry.original;
                if (actual.rawPath != recorded.rawPath || actual.cipherKey != recorded.cipherKey || actual.size != recorded.size ||
                    actual.storedSize != recorded.storedSize || actual.offset != recorded.offset || actual.flags != recorded.flags || actual.indexChecksum != recorded.indexChecksum)
                    throw ArchiveError(ErrorCode::Integrity, "清单中的原始存储元数据被修改");
            }
            present[entry.id] = true;
        }
        if (std::find(present.begin(), present.end(), false) != present.end())
            throw ArchiveError(ErrorCode::Conflict, "清单缺少原始条目；删除必须登记为 delete");
    }
    std::vector<const WorkspaceEntry*> selected;
    std::uint64_t indexSize = 0;
    for (const auto& entry : workspace.manifest.entries) if (entry.change != ChangeKind::Delete) {
        if ((entry.extracted || entry.change != ChangeKind::Keep) && Detail::SamePath(workspace.InputFile(entry), target))
            throw ArchiveError(ErrorCode::Conflict, "输出不能覆盖工作区输入文件");
        selected.push_back(&entry); indexSize += 40 + FromUtf8(entry.path).size() * 2;
    }
    if (selected.size() > 500000 || indexSize > 64 * 1024 * 1024) throw ArchiveError(ErrorCode::Bounds, "索引超出已支持的工具限制");
    const auto parameters = Detail::DeriveName(workspace.manifest.logicalDirectory, packageName);
    const auto dataBase = Detail::Align(parameters.indexOffset + Detail::Align(indexSize, 4), 1024);
    Detail::AtomicFile transaction(target, options.overwrite);
    Detail::File file(transaction.temporary, true);
    Detail::Pad(file, dataBase);
    std::vector<ArchiveEntry> built;
    std::vector<std::string> expectedHashes;
    for (const auto* record : selected) {
        job.Report("生成资源包", packageName + ": " + record->path, built.size(), selected.size());
        ArchiveEntry entry;
        entry.id = static_cast<EntryId>(built.size()); entry.path = record->path; entry.rawPath = FromUtf8(entry.path);
        entry.lookupPath = NormalizePath(entry.path); entry.flags = record->flags;
        const ArchiveEntry* old = original && record->change != ChangeKind::Add ? &original->Entry(record->id) : nullptr;
        std::optional<ContentDigest> inputDigest;
        if (record->extracted || record->change != ChangeKind::Keep) inputDigest = HashFile(workspace.InputFile(*record), job);
        const bool reuse = old && record->change == ChangeKind::Keep && entry.flags == old->flags &&
                           (!inputDigest || (!record->originalSha256.empty() && inputDigest->sha256 == record->originalSha256));
        const auto start = Detail::Align(file.Tell(), 1024);
        Detail::Pad(file, start); entry.offset = start;
        if (reuse) {
            const auto digest = ReadEntryTo(*original, old->id, {}, job);
            expectedHashes.push_back(digest.sha256);
            entry.size = old->size; entry.cipherKey = old->cipherKey; entry.storedSize = old->storedSize;
            Detail::File source(original->source); source.Seek(old->offset);
            std::array<std::uint8_t, 65536> buffer{};
            for (std::uint64_t copied = 0; copied < old->storedSize;) {
                job.Check();
                auto chunk = std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), old->storedSize - copied)));
                source.Read(chunk); file.Write(chunk); copied += chunk.size();
            }
        } else {
            if (!inputDigest) throw ArchiveError(ErrorCode::Conflict, "变更后的文件没有输入映射");
            if (inputDigest->size > UINT32_MAX) throw ArchiveError(ErrorCode::Bounds, "单文件超过 PKN v2 长度字段");
            entry.size = inputDigest->size; entry.cipherKey = inputDigest->md5;
            expectedHashes.push_back(inputDigest->sha256);
            EncodeInput(workspace.InputFile(*record), entry, file, job);
            entry.storedSize = file.Tell() - start;
        }
        if (file.Tell() > UINT32_MAX - 1023) throw ArchiveError(ErrorCode::Bounds, "输出超过客户端 32 位包容量；自动分包尚未启用");
        built.push_back(std::move(entry));
    }
    Detail::Pad(file, Detail::Align(file.Tell(), 1024));
    std::array<std::uint8_t, 12> header{};
    WriteU32(header, static_cast<std::uint32_t>(built.size()) + 2); header[4] = 2;
    WriteU32(std::span(header).subspan(5), static_cast<std::uint32_t>(built.size()));
    SnowCipher headerCipher(parameters.headerKey); headerCipher.Transform(header, true);
    file.Seek(parameters.headerOffset); file.Write(header);
    std::vector<std::uint8_t> index(static_cast<std::size_t>(Detail::Align(indexSize, 4)), 0);
    std::size_t position = 0;
    const auto append = [&](std::uint32_t value) { WriteU32(std::span(index).subspan(position, 4), value); position += 4; };
    for (const auto& entry : built) {
        append(static_cast<std::uint32_t>(entry.rawPath.size()));
        for (const auto unit : entry.rawPath) { index[position++] = static_cast<std::uint8_t>(unit); index[position++] = static_cast<std::uint8_t>(unit >> 8); }
        const auto block = static_cast<std::uint32_t>((entry.offset - dataBase) / 1024);
        std::uint32_t checksum = entry.flags + block + static_cast<std::uint32_t>(entry.size) + static_cast<std::uint32_t>(entry.storedSize);
        for (const auto value : entry.cipherKey) checksum += value;
        append(checksum); append(entry.flags); append(block); append(static_cast<std::uint32_t>(entry.size)); append(static_cast<std::uint32_t>(entry.storedSize));
        std::copy(entry.cipherKey.begin(), entry.cipherKey.end(), index.begin() + position); position += 16;
    }
    SnowCipher indexCipher(parameters.indexKey); indexCipher.Transform(index, true);
    file.Seek(parameters.indexOffset); file.Write(index); file.Flush(); file.Close();
    job.Report("验证新包", packageName, 0, built.size());
    const auto catalog = OpenArchive(transaction.temporary, {workspace.manifest.logicalDirectory, packageName}, job);
    ValidationReport report;
    if (catalog.entries.size() != built.size()) throw ArchiveError(ErrorCode::Integrity, "新包条目数不一致");
    for (const auto& entry : catalog.entries) {
        job.Report("验证新包", packageName + ": " + entry.path, report.entries, built.size());
        if (entry.path != built[entry.id].path || ReadEntryTo(catalog, entry.id, {}, job).sha256 != expectedHashes[entry.id])
            throw ArchiveError(ErrorCode::Integrity, "新包内容 SHA-256 不一致：" + entry.path);
        ++report.entries; report.decodedBytes += entry.size;
    }
    report.packageSha256 = HashFile(transaction.temporary, job).sha256;
    job.Check(); transaction.Commit();
    return report;
}
}
