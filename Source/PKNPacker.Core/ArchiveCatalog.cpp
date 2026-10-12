#include "ArchiveCatalog.h"
#include "ArchiveCodec.h"
#include <Windows.h>
#include <algorithm>
#include <limits>
#include <unordered_set>
#include <map>
#include <set>

namespace PKNPacker {
std::u16string FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw ArchiveError(ErrorCode::Bounds, "字符串过长");
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) throw ArchiveError(ErrorCode::InvalidFormat, "非法 UTF-8 字符串");
    std::u16string result(static_cast<std::size_t>(size), '\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), reinterpret_cast<wchar_t*>(result.data()), size);
    return result;
}
std::string ToUtf8(const std::u16string& text) {
    if (text.empty()) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, reinterpret_cast<const wchar_t*>(text.data()),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw ArchiveError(ErrorCode::InvalidFormat, "非法 UTF-16 路径");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, reinterpret_cast<const wchar_t*>(text.data()), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}
std::string NormalizePath(const std::string& text) {
    const auto wide = FromUtf8(text);
    if (wide.empty() || wide.size() > 0x4000) throw ArchiveError(ErrorCode::InvalidFormat, "资源路径为空或超过 16384 UTF-16 单元");
    std::string result = text;
    std::replace(result.begin(), result.end(), '\\', '/');
    if (result.front() == '/' || result.back() == '/') throw ArchiveError(ErrorCode::Conflict, "资源路径必须是相对文件路径：" + text);
    for (const auto c : result) {
        if ((static_cast<unsigned char>(c) < 32) || c == ':' || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*')
            throw ArchiveError(ErrorCode::Conflict, "资源路径包含禁止的字符：" + text);
    }
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    std::size_t start = 0;
    while (start < result.size()) {
        const auto end = result.find('/', start);
        const auto part = result.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.')
            throw ArchiveError(ErrorCode::Conflict, "资源路径可能逃逸或产生别名：" + text);
        const auto stem = part.substr(0, part.find('.'));
        if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9'))
            throw ArchiveError(ErrorCode::Conflict, "资源路径包含 Windows 保留名称：" + text);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return result;
}
void ValidatePackageName(const std::string& name) {
    const auto canonical = NormalizePath(name);
    if (canonical.find('/') != std::string::npos || !canonical.ends_with(".pkn"))
        throw ArchiveError(ErrorCode::Conflict, "包名必须是单个 .pkn 文件名");
    const auto dot = canonical.size() - 4, underscore = canonical.rfind('_', dot);
    if (underscore == std::string::npos || underscore == 0 || underscore + 1 == dot)
        throw ArchiveError(ErrorCode::Conflict, "客户端包名需要使用 Name_00000.pkn 形式");
    for (std::size_t i = underscore + 1; i < dot; ++i) if (canonical[i] < '0' || canonical[i] > '9')
        throw ArchiveError(ErrorCode::Conflict, "包名编号必须为十进制数字");
    for (const auto c : name) if (static_cast<unsigned char>(c) >= 128)
        throw ArchiveError(ErrorCode::Unsupported, "当前已验证的包名编码仅支持 ASCII；内部资源路径支持 UTF-16");
}
std::vector<ResourcePathConflict> FindResourcePathConflicts(std::span<const NamedResourcePath> paths, const ArchiveJob& job) {
    std::map<std::string, std::set<std::string>> names;
    for (const auto& path : paths) { job.Check(); names[NormalizePath(path.path)].insert(path.owner); }
    std::vector<ResourcePathConflict> result;
    for (const auto& [name, owners] : names) if (owners.size() > 1) result.push_back({name, {owners.begin(), owners.end()}});
    return result;
}
const ArchiveEntry& ArchiveCatalog::Entry(EntryId id) const {
    if (id >= entries.size() || entries[id].id != id) throw ArchiveError(ErrorCode::Bounds, "无效的条目 ID");
    return entries[id];
}
ArchiveCatalog OpenArchive(const std::filesystem::path& path, const OpenOptions& options, const ArchiveJob& job) {
    ArchiveCatalog archive;
    archive.source = std::filesystem::absolute(path).lexically_normal();
    archive.packageName = options.packageName.value_or(Detail::PathUtf8(path.filename()));
    archive.logicalDirectory = options.logicalDirectory;
    ValidatePackageName(archive.packageName);
    Detail::File file(archive.source);
    archive.fileSize = file.Size();
    archive.modifiedTime = Detail::ModifiedTime(archive.source);
    if (archive.fileSize > UINT32_MAX || archive.fileSize < 1024 || archive.fileSize % 1024)
        throw ArchiveError(ErrorCode::InvalidFormat, "当前 PKN v2 需要 1024 对齐且小于 4 GiB");
    const auto name = Detail::DeriveName(options.logicalDirectory, archive.packageName);
    archive.headerOffset = name.headerOffset; archive.indexOffset = name.indexOffset;
    std::array<std::uint8_t, 12> header{};
    file.Seek(name.headerOffset); file.Read(header);
    SnowCipher headerCipher(name.headerKey); headerCipher.Transform(header);
    const auto count = ReadU32(std::span(header).subspan(5, 4));
    archive.version = header[4];
    if (archive.version != 2 || ReadU32(header) != count + std::uint32_t(archive.version))
        throw ArchiveError(ErrorCode::InvalidFormat, "头部校验失败：包名、逻辑目录或版本不匹配");
    if (count > 500000 || name.indexOffset > archive.fileSize || std::uint64_t(count) * 42 > archive.fileSize - name.indexOffset)
        throw ArchiveError(ErrorCode::Bounds, "PKN 条目计数超出文件边界或工具上限");
    file.Seek(name.indexOffset);
    SnowCipher cipher(name.indexKey);
    std::array<std::uint8_t, 4> word{};
    unsigned pending = 4;
    const auto read = [&](std::span<std::uint8_t> output) {
        for (auto& byte : output) {
            if (pending == 4) { file.Read(word); cipher.Transform(word); pending = 0; }
            byte = word[pending++];
        }
    };
    archive.entries.reserve(count);
    std::unordered_set<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i) {
        job.Report("读取索引", archive.packageName, i, count);
        std::array<std::uint8_t, 4> lengthBytes{};
        read(lengthBytes);
        const auto length = ReadU32(lengthBytes);
        if (!length || length > 0x4000 || std::uint64_t(length) * 2 > archive.fileSize - file.Tell())
            throw ArchiveError(ErrorCode::Bounds, "索引路径长度非法");
        std::vector<std::uint8_t> raw(std::size_t(length) * 2);
        read(raw);
        ArchiveEntry entry;
        entry.id = i; entry.rawPath.resize(length);
        for (std::size_t j = 0; j < length; ++j) entry.rawPath[j] = static_cast<char16_t>(raw[j * 2] | (unsigned(raw[j * 2 + 1]) << 8));
        entry.path = ToUtf8(entry.rawPath); entry.lookupPath = NormalizePath(entry.path);
        if (!names.insert(entry.lookupPath).second) throw ArchiveError(ErrorCode::Conflict, "索引含重复路径：" + entry.path);
        std::array<std::uint8_t, 36> metadata{};
        read(metadata);
        entry.indexChecksum = ReadU32(metadata);
        entry.flags = ReadU32(std::span(metadata).subspan(4));
        entry.offset = std::uint64_t(ReadU32(std::span(metadata).subspan(8))) * 1024;
        entry.size = ReadU32(std::span(metadata).subspan(12));
        entry.storedSize = ReadU32(std::span(metadata).subspan(16));
        std::copy_n(metadata.begin() + 20, 16, entry.cipherKey.begin());
        std::uint32_t checksum = entry.flags + static_cast<std::uint32_t>(entry.offset / 1024)
                                + static_cast<std::uint32_t>(entry.size) + static_cast<std::uint32_t>(entry.storedSize);
        for (const auto value : entry.cipherKey) checksum += value;
        if (checksum != ReadU32(metadata)) throw ArchiveError(ErrorCode::Integrity, "索引校验错误：" + entry.path);
        if (entry.flags > 4) throw ArchiveError(ErrorCode::Unsupported, "未知存储标志：" + std::to_string(entry.flags));
        if ((entry.flags & 2) && entry.storedSize % 4) throw ArchiveError(ErrorCode::InvalidFormat, "完整加密数据未按 4 对齐");
        if ((entry.flags & 4) && std::min<std::uint64_t>(1024, entry.storedSize) % 4)
            throw ArchiveError(ErrorCode::InvalidFormat, "局部加密前缀未按 4 对齐");
        if (!(entry.flags & 1) && (entry.storedSize < entry.size || entry.storedSize > entry.size + 3))
            throw ArchiveError(ErrorCode::InvalidFormat, "未压缩条目的大小关系不合法");
        archive.entries.push_back(std::move(entry));
    }
    archive.dataBase = Detail::Align(file.Tell(), 1024);
    for (auto& entry : archive.entries) {
        if (archive.dataBase > archive.fileSize || entry.offset > archive.fileSize - archive.dataBase ||
            entry.storedSize > archive.fileSize - archive.dataBase - entry.offset)
            throw ArchiveError(ErrorCode::Bounds, "资源数据超出包边界：" + entry.path);
        entry.offset += archive.dataBase;
    }
    auto sorted = archive.entries;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    std::uint64_t end = archive.dataBase;
    for (const auto& entry : sorted) {
        if (entry.storedSize && entry.offset < end) throw ArchiveError(ErrorCode::Bounds, "资源存储块重叠：" + entry.path);
        end = std::max(end, entry.offset + entry.storedSize);
    }
    return archive;
}
}
