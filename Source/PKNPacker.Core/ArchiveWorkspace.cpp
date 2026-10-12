#include "ArchiveWorkspace.h"
#include "Json.h"
#include <algorithm>
#include <unordered_set>

namespace PKNPacker {
namespace {
std::string ChangeName(ChangeKind kind) {
    switch (kind) { case ChangeKind::Keep: return "keep"; case ChangeKind::Replace: return "replace";
                   case ChangeKind::Add: return "add"; case ChangeKind::Delete: return "delete"; }
    throw ArchiveError(ErrorCode::InvalidFormat, "未知变更类型");
}
ChangeKind ParseChange(const std::string& name) {
    if (name == "keep") return ChangeKind::Keep;
    if (name == "replace") return ChangeKind::Replace;
    if (name == "add") return ChangeKind::Add;
    if (name == "delete") return ChangeKind::Delete;
    throw ArchiveError(ErrorCode::InvalidFormat, "清单含未知操作");
}
void CopyInput(const std::filesystem::path& input, const std::filesystem::path& target, const ArchiveJob& job) {
    Detail::File source(input);
    Detail::AtomicFile output(target, true);
    Detail::File destination(output.temporary, true);
    std::array<std::uint8_t, 65536> buffer{};
    for (std::uint64_t done = 0; done < source.Size();) {
        job.Check();
        auto chunk = std::span(buffer).first(static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), source.Size() - done)));
        source.Read(chunk); destination.Write(chunk); done += chunk.size();
    }
    job.Check(); destination.Flush(); destination.Close(); output.Commit();
}
WorkspaceEntry& FindEntry(WorkspaceManifest& manifest, EntryId id) {
    const auto it = std::find_if(manifest.entries.begin(), manifest.entries.end(), [=](const auto& entry) { return entry.id == id; });
    if (it == manifest.entries.end()) throw ArchiveError(ErrorCode::Bounds, "清单中没有此条目");
    return *it;
}
}
ArchiveWorkspace ArchiveWorkspace::Load(const std::filesystem::path& directory) {
    ArchiveWorkspace workspace;
    workspace.root = std::filesystem::absolute(directory).lexically_normal();
    Detail::EnsureSafeRoot(workspace.root);
    try {
        const auto data = Json::parse(Detail::ReadText(workspace.root / "WorkspaceManifest.json"));
        if (data.at("schema") != "PKNPacker.Workspace" || data.at("version") != WorkspaceManifest::CurrentVersion ||
            data.at("format") != "BubbleFighter.PKN.v2") throw ArchiveError(ErrorCode::Unsupported, "不支持此工作区清单版本");
        auto& m = workspace.manifest;
        m.source = Detail::Utf8Path(data.at("source").get<std::string>());
        m.sourceSha256 = data.value("source_sha256", "");
        m.originalPackageName = data.at("original_package_name").get<std::string>();
        m.packageName = data.at("package_name").get<std::string>();
        m.logicalDirectory = data.at("logical_directory").get<std::string>();
        m.complete = data.at("complete").get<bool>();
        ValidatePackageName(m.packageName);
        if (data.at("entries").size() > 500000) throw ArchiveError(ErrorCode::Bounds, "清单条目过多");
        std::unordered_set<std::uint32_t> ids;
        for (const auto& e : data.at("entries")) {
            WorkspaceEntry entry;
            entry.id = e.at("id").get<std::uint32_t>(); entry.path = e.at("path").get<std::string>();
            entry.file = e.at("file").get<std::string>(); entry.flags = e.at("flags").get<std::uint32_t>();
            entry.originalSha256 = e.at("original_sha256").get<std::string>();
            entry.change = ParseChange(e.at("operation").get<std::string>());
            entry.extracted = e.at("extracted").get<bool>();
            if (e.contains("original") && !e.at("original").is_null()) {
                const auto& old = e.at("original"); ArchiveEntry original;
                original.id = entry.id; original.path = entry.path; original.lookupPath = NormalizePath(entry.path);
                const auto raw = Unhex(old.at("raw_path_utf16le").get<std::string>());
                if (raw.size() % 2) throw ArchiveError(ErrorCode::InvalidFormat, "原路径 UTF-16 表示无效");
                for (std::size_t n = 0; n < raw.size(); n += 2) original.rawPath.push_back(static_cast<char16_t>(raw[n] | (unsigned(raw[n + 1]) << 8)));
                original.flags = old.at("flags").get<std::uint32_t>(); original.indexChecksum = old.at("index_checksum").get<std::uint32_t>();
                original.offset = old.at("absolute_offset").get<std::uint64_t>(); original.size = old.at("size").get<std::uint64_t>(); original.storedSize = old.at("stored_size").get<std::uint64_t>();
                const auto key = Unhex(old.at("cipher_key").get<std::string>());
                if (key.size() != 16 || ToUtf8(original.rawPath) != entry.path) throw ArchiveError(ErrorCode::InvalidFormat, "原始元数据与文件身份不一致");
                std::copy(key.begin(), key.end(), original.cipherKey.begin()); entry.original = original;
            }
            NormalizePath(entry.path); NormalizePath(entry.file);
            if (entry.flags > 4 || !ids.insert(entry.id).second) throw ArchiveError(ErrorCode::InvalidFormat, "清单存储标志或 ID 不合法");
            m.entries.push_back(std::move(entry));
        }
        return workspace;
    } catch (const Json::exception& error) { throw ArchiveError(ErrorCode::InvalidFormat, std::string("清单解析失败：") + error.what()); }
}
void ArchiveWorkspace::Save() const {
    Json data = {{"schema", "PKNPacker.Workspace"}, {"version", WorkspaceManifest::CurrentVersion},
                 {"format", "BubbleFighter.PKN.v2"}, {"source", Detail::PathUtf8(manifest.source)},
                 {"source_sha256", manifest.sourceSha256}, {"original_package_name", manifest.originalPackageName},
                 {"package_name", manifest.packageName}, {"logical_directory", manifest.logicalDirectory},
                 {"complete", manifest.complete}, {"entries", Json::array()}};
    for (const auto& entry : manifest.entries) {
        Json record = {{"id", entry.id}, {"path", entry.path}, {"file", entry.file}, {"flags", entry.flags},
                       {"original_sha256", entry.originalSha256}, {"operation", ChangeName(entry.change)}, {"extracted", entry.extracted}, {"original", nullptr}};
        if (entry.original) {
            const auto& original = *entry.original; std::vector<std::uint8_t> raw;
            for (const auto unit : original.rawPath) { raw.push_back(static_cast<std::uint8_t>(unit)); raw.push_back(static_cast<std::uint8_t>(unit >> 8)); }
            record["original"] = {{"raw_path_utf16le", Hex(raw)}, {"flags", original.flags}, {"index_checksum", original.indexChecksum},
                                   {"absolute_offset", original.offset}, {"size", original.size}, {"stored_size", original.storedSize}, {"cipher_key", Hex(original.cipherKey)}};
        }
        data["entries"].push_back(std::move(record));
    }
    Detail::WriteTextAtomic(root / "WorkspaceManifest.json", data.dump(2) + "\n");
}
ArchiveWorkspace ArchiveWorkspace::FromArchive(const ArchiveCatalog& archive, const std::filesystem::path& directory, const ArchiveJob& job) {
    ArchiveWorkspace workspace;
    workspace.root = std::filesystem::absolute(directory).lexically_normal();
    if (std::filesystem::exists(workspace.root / "WorkspaceManifest.json")) throw ArchiveError(ErrorCode::Conflict, "工作区已存在，请打开已有清单");
    Detail::EnsureSafeRoot(workspace.root);
    auto& m = workspace.manifest;
    m.source = archive.source; m.originalPackageName = m.packageName = archive.packageName; m.logicalDirectory = archive.logicalDirectory;
    m.sourceSha256 = HashFile(archive.source, job).sha256;
    for (const auto& entry : archive.entries)
        m.entries.push_back({entry.id, entry.path, "Files/" + entry.path, "", entry.flags, ChangeKind::Keep, false, entry});
    workspace.Save();
    return workspace;
}
ArchiveWorkspace ArchiveWorkspace::FromDirectory(const std::filesystem::path& directory, const std::string& nameSpace,
                                                 const std::string& packageName, const std::filesystem::path& destination,
                                                 std::uint32_t flags, const ArchiveJob& job) {
    ValidatePackageName(packageName); NormalizePath(nameSpace);
    if (flags > 4) throw ArchiveError(ErrorCode::Unsupported, "未知的新建存储模式");
    const auto inputRoot = std::filesystem::absolute(directory).lexically_normal();
    const auto workspaceRoot = std::filesystem::absolute(destination).lexically_normal();
    for (auto ancestor = workspaceRoot; !ancestor.empty();) {
        if (Detail::SamePath(ancestor, inputRoot)) throw ArchiveError(ErrorCode::Conflict, "工作区必须放在输入目录之外");
        const auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = parent;
    }
    if (std::filesystem::exists(workspaceRoot / "WorkspaceManifest.json")) throw ArchiveError(ErrorCode::Conflict, "目标工作区已存在");
    ArchiveWorkspace workspace;
    workspace.root = workspaceRoot;
    workspace.manifest.originalPackageName = workspace.manifest.packageName = packageName;
    Detail::EnsureSafeRoot(workspace.root);
    std::vector<std::filesystem::path> paths;
    for (const auto& item : std::filesystem::recursive_directory_iterator(inputRoot)) {
        job.Check();
        const auto relative = Detail::PathUtf8(item.path().lexically_relative(inputRoot));
        Detail::SafeChild(inputRoot, relative, false);
        if (item.is_regular_file() && item.path().filename() != "WorkspaceManifest.json") paths.push_back(item.path());
    }
    std::sort(paths.begin(), paths.end());
    std::unordered_set<std::string> names;
    for (const auto& path : paths) {
        const auto relative = Detail::PathUtf8(path.lexically_relative(inputRoot));
        auto internal = nameSpace + "/" + relative; std::replace(internal.begin(), internal.end(), '\\', '/');
        if (!names.insert(NormalizePath(internal)).second) throw ArchiveError(ErrorCode::Conflict, "新建目录存在规范化路径冲突");
        if (workspace.manifest.entries.size() >= 500000) throw ArchiveError(ErrorCode::Bounds, "新建包条目过多");
        const auto id = static_cast<EntryId>(workspace.manifest.entries.size()); const auto file = "Changes/" + std::to_string(id) + ".bin";
        job.Report("准备新包", internal, id, paths.size()); CopyInput(path, Detail::SafeChild(workspace.root, file), job);
        workspace.manifest.entries.push_back({id, internal, file, "", flags, ChangeKind::Add, true, {}});
    }
    workspace.Save();
    return workspace;
}
std::filesystem::path ArchiveWorkspace::InputFile(const WorkspaceEntry& entry) const {
    return Detail::SafeChild(root, entry.file, false);
}
void ArchiveWorkspace::Replace(EntryId id, const std::filesystem::path& input, const ArchiveJob& job) {
    auto& entry = FindEntry(manifest, id);
    if (entry.change == ChangeKind::Delete) throw ArchiveError(ErrorCode::Conflict, "请先撤销删除，再替换文件");
    const auto staged = "Changes/" + std::to_string(id) + ".bin";
    CopyInput(input, Detail::SafeChild(root, staged), job);
    entry.file = staged; entry.extracted = true;
    if (entry.change != ChangeKind::Add) entry.change = ChangeKind::Replace;
    Save();
}
void ArchiveWorkspace::Add(const std::string& path, const std::filesystem::path& input, std::uint32_t flags, const ArchiveJob& job) {
    const auto lookup = NormalizePath(path);
    for (const auto& entry : manifest.entries) if (entry.change != ChangeKind::Delete && NormalizePath(entry.path) == lookup)
        throw ArchiveError(ErrorCode::Conflict, "新增路径与已有文件冲突：" + path);
    if (flags > 4 || manifest.entries.size() >= 500000) throw ArchiveError(ErrorCode::Bounds, "存储模式或条目数量超出限制");
    EntryId id = 0;
    for (const auto& entry : manifest.entries) id = std::max(id, entry.id + 1);
    const auto file = "Changes/" + std::to_string(id) + ".bin";
    CopyInput(input, Detail::SafeChild(root, file), job);
    manifest.entries.push_back({id, path, file, "", flags, ChangeKind::Add, true, {}});
    Save();
}
void ArchiveWorkspace::Delete(EntryId id) {
    auto& entry = FindEntry(manifest, id);
    if (entry.change == ChangeKind::Add) manifest.entries.erase(std::remove_if(manifest.entries.begin(), manifest.entries.end(),
         [=](const auto& value) { return value.id == id; }), manifest.entries.end());
    else entry.change = ChangeKind::Delete;
    Save();
}
void ArchiveWorkspace::Undo(EntryId id) {
    auto& entry = FindEntry(manifest, id);
    if (entry.change == ChangeKind::Add) { Delete(id); return; }
    entry.change = ChangeKind::Keep; entry.file = "Files/" + entry.path;
    entry.extracted = std::filesystem::exists(InputFile(entry));
    Save();
}
ChangeSummary ArchiveWorkspace::CheckChanges(const ArchiveJob& job) const {
    ChangeSummary result;
    if (!manifest.complete) result.conflicts.push_back("这是部分解包清单，不能重建完整包");
    std::unordered_set<std::string> names;
    for (const auto& entry : manifest.entries) {
        job.Check();
        if (entry.change == ChangeKind::Delete) { result.deleted.push_back(entry.path); continue; }
        try {
            if (!names.insert(NormalizePath(entry.path)).second) result.conflicts.push_back("重复路径：" + entry.path);
            if (entry.extracted || entry.change != ChangeKind::Keep) {
                const auto input = InputFile(entry);
                if (!std::filesystem::is_regular_file(input)) { result.conflicts.push_back("缺少文件，删除须显式登记：" + entry.path); continue; }
                if (entry.change == ChangeKind::Add) result.added.push_back(entry.path);
                else if (entry.change == ChangeKind::Replace || entry.originalSha256.empty() || HashFile(input, job).sha256 != entry.originalSha256)
                    result.replaced.push_back(entry.path);
            } else if (manifest.source.empty()) result.conflicts.push_back("文件没有原包来源：" + entry.path);
        } catch (const ArchiveError& error) {
            if (error.Code() == ErrorCode::Cancelled) throw;
            result.conflicts.push_back(error.what());
        }
    }
    return result;
}
JobResult ExtractEntries(const ArchiveCatalog& archive, const std::vector<EntryId>& ids, const std::filesystem::path& directory,
                         const ExtractOptions& options, const ArchiveJob& job) {
    JobResult result;
    std::unordered_set<EntryId> unique;
    for (const auto id : ids) {
        archive.Entry(id);
        if (!unique.insert(id).second) throw ArchiveError(ErrorCode::Conflict, "解包选择包含重复条目 ID");
    }
    std::optional<ArchiveWorkspace> workspace;
    if (options.createManifest) {
        if (std::filesystem::exists(directory / "WorkspaceManifest.json")) {
            workspace = ArchiveWorkspace::Load(directory);
            if (!Detail::SamePath(workspace->manifest.source, archive.source) || workspace->manifest.sourceSha256 != HashFile(archive.source, job).sha256)
                throw ArchiveError(ErrorCode::Conflict, "工作区属于其他资源包或原包已变化");
            for (const auto& record : workspace->manifest.entries) if (record.change != ChangeKind::Keep)
                throw ArchiveError(ErrorCode::Conflict, "工作区存在待提交操作，请使用新的解包目录或先撤销操作");
        } else {
            workspace = ArchiveWorkspace::FromArchive(archive, directory, job);
            workspace->manifest.complete = false;
        }
    }
    for (const auto id : ids) {
        const auto& entry = archive.Entry(id);
        ItemResult item{entry.path};
        try {
            job.Report("解包", archive.packageName + ": " + entry.path, result.items.size(), ids.size());
            const auto target = Detail::SafeChild(directory, (options.createManifest ? "Files/" : "") + entry.path);
            if (std::filesystem::exists(target) && !options.overwrite) {
                item.status = ItemStatus::Skipped; item.message = "目标已存在";
                if (workspace) {
                    auto& record = FindEntry(workspace->manifest, id);
                    record.extracted = true;
                    // Record the original hash, even when the existing file differs.
                    record.originalSha256 = ReadEntryTo(archive, id, {}, job).sha256;
                }
            } else {
                const auto digest = ExportEntry(archive, id, target, options.overwrite, job);
                if (workspace) {
                    auto& record = FindEntry(workspace->manifest, id);
                    record.extracted = true; record.originalSha256 = digest.sha256;
                }
            }
        } catch (const ArchiveError& error) {
            item.status = error.Code() == ErrorCode::Cancelled ? ItemStatus::Cancelled : ItemStatus::Failed; item.message = error.what();
        } catch (const std::exception& error) { item.status = ItemStatus::Failed; item.message = error.what(); }
        result.items.push_back(std::move(item));
        if (job.cancelled && job.cancelled->load()) break;
    }
    if (workspace) {
        if (!result.Succeeded() || result.items.size() != ids.size()) workspace->manifest.complete = false;
        else if (std::all_of(workspace->manifest.entries.begin(), workspace->manifest.entries.end(), [](const auto& record) { return record.extracted; }))
            workspace->manifest.complete = true;
        workspace->Save();
    }
    return result;
}
}
