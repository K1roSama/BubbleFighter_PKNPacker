#pragma once
#include "ArchiveJob.h"
#include <filesystem>
#include <span>
#include <string>
#include <cstdint>
#include <vector>

namespace PKNPacker::Detail {
class File {
public:
    explicit File(const std::filesystem::path& path, bool create = false);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    std::uint64_t Size() const;
    std::uint64_t Tell() const;
    void Seek(std::uint64_t offset);
    void Read(std::span<std::uint8_t> bytes);
    void Write(std::span<const std::uint8_t> bytes);
    void Flush();
    void Close();
private:
    void* handle_ = nullptr;
};
class AtomicFile {
public:
    AtomicFile(const std::filesystem::path& target, bool overwrite);
    ~AtomicFile();
    std::filesystem::path temporary, target;
    void Commit();
private:
    bool overwrite_, committed_ = false;
};
void EnsureSafeRoot(const std::filesystem::path& root);
std::filesystem::path SafeChild(const std::filesystem::path& root, const std::string& relative, bool createDirectories = true);
std::string PathUtf8(const std::filesystem::path& path);
std::filesystem::path Utf8Path(const std::string& path);
std::int64_t ModifiedTime(const std::filesystem::path& path);
bool SamePath(const std::filesystem::path& first, const std::filesystem::path& second);
std::string ReadText(const std::filesystem::path& path, std::uint64_t limit = 64 * 1024 * 1024);
void WriteTextAtomic(const std::filesystem::path& path, const std::string& text);
}
