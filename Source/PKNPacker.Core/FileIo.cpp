#include "FileIo.h"
#include "ArchiveCatalog.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <limits>

namespace PKNPacker::Detail {
namespace {
[[noreturn]] void IoError(const std::string& action) {
    throw ArchiveError(ErrorCode::Io, action + " (Windows " + std::to_string(GetLastError()) + ")");
}
void RejectReparse(const std::filesystem::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw ArchiveError(ErrorCode::Conflict, "拒绝符号链接或目录联接：" + PathUtf8(path));
}
void GuardOriginalInputs(const std::filesystem::path& path) {
    static const auto clientRoot = [] {
        std::wstring filename(32768, L'\0'); const auto count = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size())); filename.resize(count);
        auto directory = std::filesystem::path(filename).parent_path();
        while (!directory.empty() && directory != directory.parent_path()) {
            if (directory.filename() == "PKNPacker" && std::filesystem::exists(directory.parent_path() / "BubbleFighter.exe")) return directory.parent_path();
            directory = directory.parent_path();
        }
        return std::filesystem::path{};
    }();
    if (clientRoot.empty()) return;
    const auto full = std::filesystem::absolute(path).lexically_normal();
    const auto fullText = full.wstring(), prefix = clientRoot.wstring() + L"\\";
    if (fullText.size() <= prefix.size() || CompareStringOrdinal(fullText.data(), static_cast<int>(prefix.size()),
        prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) return;
    const auto relative = std::filesystem::path(fullText.substr(prefix.size()));
    if (relative.empty() || relative.begin() == relative.end()) return;
    auto first = relative.begin()->wstring();
    for (auto& c : first) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    if (first == L"packs" || first == L"static_analysis" || first == L"data" || first == L"blackcipher" ||
        first == L"sounds" || first == L"dwarf" || first == L"promotion" || first == L"voicechat" || first == L"download" ||
        (!relative.has_parent_path() && std::filesystem::exists(full)))
        throw ArchiveError(ErrorCode::Conflict, "输出位置属于只读客户端输入：" + PathUtf8(path));
}
}
std::string PathUtf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
std::filesystem::path Utf8Path(const std::string& path) {
    return std::filesystem::path(FromUtf8(path));
}
File::File(const std::filesystem::path& path, bool create) {
    handle_ = CreateFileW(path.c_str(), create ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                          create ? 0 : FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) { handle_ = nullptr; IoError("无法打开文件：" + PathUtf8(path)); }
}
File::~File() { Close(); }
void File::Close() { if (handle_) { CloseHandle(handle_); handle_ = nullptr; } }
std::uint64_t File::Size() const {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle_, &size)) IoError("读取文件大小失败");
    return static_cast<std::uint64_t>(size.QuadPart);
}
std::uint64_t File::Tell() const {
    LARGE_INTEGER zero{}, position{};
    if (!SetFilePointerEx(handle_, zero, &position, FILE_CURRENT)) IoError("查询偏移失败");
    return static_cast<std::uint64_t>(position.QuadPart);
}
void File::Seek(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw ArchiveError(ErrorCode::Bounds, "文件偏移溢出");
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) IoError("定位文件失败");
}
void File::Read(std::span<std::uint8_t> bytes) {
    while (!bytes.empty()) {
        DWORD actual = 0;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        if (!ReadFile(handle_, bytes.data(), amount, &actual, nullptr)) IoError("读取文件失败");
        if (actual != amount) throw ArchiveError(ErrorCode::Bounds, "文件内容被截断");
        bytes = bytes.subspan(actual);
    }
}
void File::Write(std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        DWORD actual = 0;
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1024 * 1024));
        if (!WriteFile(handle_, bytes.data(), amount, &actual, nullptr) || actual != amount) IoError("写入文件失败");
        bytes = bytes.subspan(actual);
    }
}
void File::Flush() { if (!FlushFileBuffers(handle_)) IoError("刷新文件失败"); }
AtomicFile::AtomicFile(const std::filesystem::path& destination, bool overwrite) : target(destination), overwrite_(overwrite) {
    GuardOriginalInputs(target);
    EnsureSafeRoot(target.parent_path());
    RejectReparse(target);
    if (!overwrite && std::filesystem::exists(target)) throw ArchiveError(ErrorCode::Conflict, "输出已存在：" + PathUtf8(target));
    static std::atomic_uint64_t sequence = 0;
    temporary = target.parent_path() / (target.filename().wstring() + L".pkntmp." + std::to_wstring(GetCurrentProcessId())
                 + L"." + std::to_wstring(sequence.fetch_add(1)));
}
AtomicFile::~AtomicFile() {
    if (!committed_) { std::error_code error; std::filesystem::remove(temporary, error); }
}
void AtomicFile::Commit() {
    GuardOriginalInputs(target);
    RejectReparse(target);
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH | (overwrite_ ? MOVEFILE_REPLACE_EXISTING : 0)))
        IoError("提交输出失败：" + PathUtf8(target));
    committed_ = true;
}
void EnsureSafeRoot(const std::filesystem::path& root) {
    const auto full = std::filesystem::absolute(root).lexically_normal();
    if (!std::filesystem::exists(full)) GuardOriginalInputs(full);
    auto current = full.root_path();
    for (const auto& part : full.relative_path()) {
        current /= part;
        RejectReparse(current);
    }
    std::filesystem::create_directories(full);
    RejectReparse(full);
}
std::filesystem::path SafeChild(const std::filesystem::path& root, const std::string& relative, bool createDirectories) {
    NormalizePath(relative); // Validate before constructing a native path.
    const auto fullRoot = std::filesystem::absolute(root).lexically_normal();
    auto target = fullRoot / Utf8Path(relative);
    auto current = fullRoot;
    RejectReparse(current);
    for (const auto& part : Utf8Path(relative)) { current /= part; RejectReparse(current); }
    if (createDirectories) EnsureSafeRoot(target.parent_path());
    return target;
}
std::int64_t ModifiedTime(const std::filesystem::path& path) {
    return std::filesystem::last_write_time(path).time_since_epoch().count();
}
bool SamePath(const std::filesystem::path& first, const std::filesystem::path& second) {
    const auto a = std::filesystem::absolute(first).lexically_normal().wstring(), b = std::filesystem::absolute(second).lexically_normal().wstring();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::string ReadText(const std::filesystem::path& path, std::uint64_t limit) {
    File file(path);
    if (file.Size() > limit) throw ArchiveError(ErrorCode::Bounds, "文本文件超出大小限制");
    std::string text(static_cast<std::size_t>(file.Size()), '\0');
    file.Read({reinterpret_cast<std::uint8_t*>(text.data()), text.size()});
    return text;
}
void WriteTextAtomic(const std::filesystem::path& path, const std::string& text) {
    AtomicFile output(path, true);
    File file(output.temporary, true);
    file.Write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    file.Flush();
    file.Close();
    output.Commit();
}
}
