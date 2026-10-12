#pragma once
#include "ArchiveCodec.h"
#include <optional>

namespace PKNPacker {
enum class ChangeKind { Keep, Replace, Add, Delete };
struct WorkspaceEntry {
    std::uint32_t id = 0;
    std::string path, file, originalSha256;
    std::uint32_t flags = 3;
    ChangeKind change = ChangeKind::Keep;
    bool extracted = false;
    std::optional<ArchiveEntry> original;
};
struct WorkspaceManifest {
    static constexpr unsigned CurrentVersion = 1;
    std::filesystem::path source;
    std::string originalPackageName, packageName, logicalDirectory = "packs", sourceSha256;
    bool complete = true;
    std::vector<WorkspaceEntry> entries;
};
struct ChangeSummary {
    std::vector<std::string> added, replaced, deleted, conflicts;
    bool Valid() const { return conflicts.empty(); }
};
class ArchiveWorkspace {
public:
    std::filesystem::path root;
    WorkspaceManifest manifest;
    static ArchiveWorkspace Load(const std::filesystem::path& root);
    static ArchiveWorkspace FromArchive(const ArchiveCatalog& archive, const std::filesystem::path& root, const ArchiveJob& job = {});
    static ArchiveWorkspace FromDirectory(const std::filesystem::path& directory, const std::string& nameSpace,
                                          const std::string& packageName, const std::filesystem::path& root,
                                          std::uint32_t flags = 3, const ArchiveJob& job = {});
    void Save() const;
    void Replace(EntryId id, const std::filesystem::path& input, const ArchiveJob& job = {});
    void Add(const std::string& path, const std::filesystem::path& input, std::uint32_t flags = 3, const ArchiveJob& job = {});
    void Delete(EntryId id);
    void Undo(EntryId id);
    ChangeSummary CheckChanges(const ArchiveJob& job = {}) const;
    std::filesystem::path InputFile(const WorkspaceEntry& entry) const;
};
struct ExtractOptions { bool overwrite = false; bool createManifest = true; };
JobResult ExtractEntries(const ArchiveCatalog& archive, const std::vector<EntryId>& ids,
                          const std::filesystem::path& root, const ExtractOptions& options = {}, const ArchiveJob& job = {});
struct BuildOptions { bool overwrite = false; std::optional<std::string> packageName; };
ValidationReport BuildArchive(const ArchiveWorkspace& workspace, const std::filesystem::path& target,
                              const BuildOptions& options = {}, const ArchiveJob& job = {});
}
