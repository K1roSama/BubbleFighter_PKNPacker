#include "ArchiveWorkspace.h"
#include "Json.h"
#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <set>

using namespace PKNPacker;
namespace {
std::atomic_bool* cancellation = nullptr;
BOOL WINAPI CancelHandler(DWORD event) {
    if ((event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) && cancellation) { cancellation->store(true); return TRUE; }
    return FALSE;
}
struct Arguments {
    std::string command;
    std::vector<std::string> inputs;
    std::map<std::string, std::vector<std::string>> options;
    bool Has(const std::string& option) const { return options.contains(option); }
    std::string Get(const std::string& option, std::string fallback = {}) const {
        const auto found = options.find(option);
        return found == options.end() || found->second.empty() ? fallback : found->second.back();
    }
    const std::vector<std::string>& Values(const std::string& option) const {
        static const std::vector<std::string> empty;
        const auto found = options.find(option); return found == options.end() ? empty : found->second;
    }
};
Arguments Parse(int count, wchar_t** values) {
    Arguments args;
    const std::set<std::string> switches{"--overwrite", "--no-manifest", "--dry-run", "--quiet", "--help"};
    const std::set<std::string> options{"--output", "--entry", "--id", "--prefix", "--ext", "--logical-dir", "--report",
         "--target", "--name", "--from-dir", "--namespace", "--workspace", "--mode", "--replace", "--add", "--delete", "--undo", "--jobs"};
    for (int i = 1; i < count; ++i) {
        const auto text = ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(values[i])));
        if (args.command.empty()) { args.command = text; continue; }
        if (switches.contains(text)) args.options[text];
        else if (options.contains(text)) {
            if (++i == count) throw ArchiveError(ErrorCode::Conflict, "选项缺少参数：" + text);
            args.options[text].push_back(ToUtf8(std::u16string(reinterpret_cast<const char16_t*>(values[i]))));
        } else if (text.starts_with("--")) throw ArchiveError(ErrorCode::Conflict, "未知选项：" + text);
        else args.inputs.push_back(text);
    }
    return args;
}
unsigned Number(const std::string& text) {
    unsigned value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw ArchiveError(ErrorCode::Conflict, "非法数字：" + text);
    return value;
}
std::vector<std::filesystem::path> Packages(const Arguments& args) {
    std::vector<std::filesystem::path> paths;
    for (const auto& input : args.inputs) {
        const auto path = Detail::Utf8Path(input);
        if (std::filesystem::is_directory(path)) {
            for (const auto& file : std::filesystem::directory_iterator(path)) if (file.is_regular_file()) {
                auto extension = Detail::PathUtf8(file.path().extension());
                std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
                if (extension == ".pkn") paths.push_back(file.path());
            }
        } else paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end()); paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.empty()) throw ArchiveError(ErrorCode::Conflict, "没有指定 PKN 文件或包含 PKN 的目录");
    return paths;
}
std::vector<EntryId> Select(const ArchiveCatalog& archive, const Arguments& args) {
    std::vector<EntryId> selected;
    const bool filter = args.Has("--entry") || args.Has("--id") || args.Has("--prefix") || args.Has("--ext");
    std::set<std::string> exact;
    for (const auto& path : args.Values("--entry")) exact.insert(NormalizePath(path));
    std::set<unsigned> ids;
    for (const auto& id : args.Values("--id")) ids.insert(Number(id));
    for (const auto& entry : archive.entries) {
        bool match = !filter || exact.contains(entry.lookupPath) || ids.contains(entry.id);
        for (const auto& path : args.Values("--prefix")) {
            auto prefix = NormalizePath(path);
            if (entry.lookupPath == prefix || entry.lookupPath.starts_with(prefix + "/")) match = true;
        }
        for (auto extension : args.Values("--ext")) {
            if (!extension.starts_with('.')) extension = "." + extension;
            std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
            if (entry.lookupPath.ends_with(extension)) match = true;
        }
        if (match) selected.push_back(entry.id);
    }
    if (filter && selected.empty()) throw ArchiveError(ErrorCode::Conflict, "选择器没有匹配到资源");
    return selected;
}
std::string Status(ItemStatus status) {
    switch (status) { case ItemStatus::Success: return "success"; case ItemStatus::Skipped: return "skipped";
                     case ItemStatus::Failed: return "failed"; case ItemStatus::Cancelled: return "cancelled"; }
    return "failed";
}
Json Results(const JobResult& result) {
    Json items = Json::array();
    for (const auto& entry : result.items) items.push_back({{"item", entry.item}, {"status", Status(entry.status)}, {"message", entry.message}});
    return items;
}
void ApplyEdits(ArchiveWorkspace& workspace, const Arguments& args, const ArchiveJob& job) {
    const auto find = [&](const std::string& path) {
        const auto canonical = NormalizePath(path);
        for (const auto& e : workspace.manifest.entries) if (NormalizePath(e.path) == canonical) return e.id;
        throw ArchiveError(ErrorCode::Conflict, "工作区找不到资源：" + path);
    };
    for (const auto& path : args.Values("--undo")) workspace.Undo(find(path));
    for (const auto& assignment : args.Values("--replace")) {
        const auto split = assignment.find('=');
        if (split == std::string::npos) throw ArchiveError(ErrorCode::Conflict, "替换格式为 内部路径=外部文件");
        workspace.Replace(find(assignment.substr(0, split)), Detail::Utf8Path(assignment.substr(split + 1)), job);
    }
    for (const auto& assignment : args.Values("--add")) {
        const auto split = assignment.find('=');
        if (split == std::string::npos) throw ArchiveError(ErrorCode::Conflict, "新增格式为 内部路径=外部文件");
        workspace.Add(assignment.substr(0, split), Detail::Utf8Path(assignment.substr(split + 1)), Number(args.Get("--mode", "3")), job);
    }
    for (const auto& path : args.Values("--delete")) workspace.Delete(find(path));
}
void Usage() {
    std::cout << "PKNPacker.Cli — BubbleFighter PKN v2\n"
      "  list <包...|Packs目录> [--entry 路径] [--prefix 目录] [--ext .xml]\n"
      "  extract <包...|目录> --output 工作区根目录 [--overwrite] [--no-manifest]\n"
      "    单文件：--entry 路径 --target 目标文件；ID 选择：--id 0\n"
      "  pack <工作区...> --output 输出目录 [--name Name_00000.pkn] [--overwrite]\n"
      "    编辑：--replace 内部路径=文件 --add 内部路径=文件 --delete 路径 --undo 路径\n"
      "    检查：--dry-run；新建：--from-dir 目录 --namespace Config --name Name_00000.pkn\n"
      "      --workspace 新工作区 --output 输出目录 [--mode 0|1|2|3|4]\n"
      "  verify <包...|目录>\n"
      "  共用：--logical-dir packs --jobs 2 --report 结果.json --quiet\n"
      "  退出码：0 成功/跳过，1 任务失败，2 用法错误，130 取消。Ctrl+C 取消当前批次。\n";
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Json report = {{"schema", "PKNPacker.JobReport"}, {"version", 1}, {"results", Json::array()}};
    Arguments args;
    ArchiveJob job;
    cancellation = job.cancelled.get(); SetConsoleCtrlHandler(CancelHandler, TRUE);
    int exitCode = 0;
    try {
        args = Parse(argc, argv);
        if (args.command.empty() || args.command == "--help" || args.Has("--help")) { Usage(); return 0; }
        if (args.Has("--report")) {
            const auto reportPath = Detail::Utf8Path(args.Get("--report"));
            auto filename = Detail::PathUtf8(reportPath.filename());
            std::transform(filename.begin(), filename.end(), filename.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
            const auto reject = [&](const std::string& reason) { args.options.erase("--report"); throw ArchiveError(ErrorCode::Conflict, reason); };
            if (!filename.ends_with(".json") || filename == "workspacemanifest.json") reject("结果报告须使用独立的 .json 文件，不能覆盖工作区清单");
            if (args.Has("--target") && Detail::SamePath(reportPath, Detail::Utf8Path(args.Get("--target")))) reject("报告不能覆盖单文件导出内容");
            for (const auto* option : {"--replace", "--add"}) for (const auto& value : args.Values(option)) {
                const auto split = value.find('=');
                if (split != std::string::npos && Detail::SamePath(reportPath, Detail::Utf8Path(value.substr(split + 1)))) reject("报告不能覆盖导入的原文件");
            }
            if (args.command == "pack") for (const auto& input : args.inputs) {
                std::optional<ArchiveWorkspace> workspace;
                try { workspace = ArchiveWorkspace::Load(Detail::Utf8Path(input)); } catch (const std::exception&) { continue; }
                for (const auto& entry : workspace->manifest.entries) if (entry.extracted && Detail::SamePath(reportPath, workspace->InputFile(entry))) reject("报告不能覆盖工作区资源文件");
            }
            if (args.command == "extract" && args.Has("--output") && !args.Has("--target")) for (const auto& path : Packages(args)) {
                std::optional<ArchiveCatalog> archive;
                try { archive = OpenArchive(path, {args.Get("--logical-dir", "packs"), {}}, job); } catch (const std::exception&) { continue; }
                for (const auto id : Select(*archive, args)) {
                    const auto target = Detail::Utf8Path(args.Get("--output")) / path.stem() / Detail::Utf8Path((args.Has("--no-manifest") ? "" : "Files/") + archive->Entry(id).path);
                    if (Detail::SamePath(reportPath, target)) reject("报告不能覆盖本次导出的资源文件");
                }
            }
        }
        std::mutex progressMutex, reportMutex;
        if (!args.Has("--quiet")) job.progress = [&](const JobProgress& progress) {
            // Throttle per-entry progress on large catalogs. stderr keeps stdout machine-readable.
            if (progress.completed % 100 == 0 || progress.completed + 1 == progress.total) {
                std::lock_guard lock(progressMutex);
                std::cerr << progress.stage << " " << progress.completed << "/" << progress.total << " " << progress.item << "\n";
            }
        };
        const unsigned concurrency = Number(args.Get("--jobs", "2"));
        if (!concurrency || concurrency > 8) throw ArchiveError(ErrorCode::Conflict, "--jobs 范围为 1..8");
        std::vector<std::pair<std::string, std::function<void()>>> tasks;
        const auto append = [&](Json record) { std::lock_guard lock(reportMutex); report["results"].push_back(std::move(record)); };
        if (args.command == "list" || args.command == "extract" || args.command == "verify") {
            const auto paths = Packages(args);
            if (args.Has("--target") && paths.size() != 1) throw ArchiveError(ErrorCode::Conflict, "指定单文件目标时只允许一个包");
            if (args.command == "extract" && !args.Has("--output") && !args.Has("--target")) throw ArchiveError(ErrorCode::Conflict, "解包需要 --output 或 --target");
            if (args.command == "extract" && !args.Has("--target")) {
                std::set<std::string> roots;
                for (const auto& path : paths) {
                    auto name = Detail::PathUtf8(path.stem());
                    std::transform(name.begin(), name.end(), name.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
                    if (!roots.insert(name).second) throw ArchiveError(ErrorCode::Conflict, "不同输入包会写入同一解包目录，请分开处理：" + name);
                }
            }
            for (const auto& path : paths) tasks.emplace_back(Detail::PathUtf8(path), [&, path] {
                OpenOptions open; open.logicalDirectory = args.Get("--logical-dir", "packs");
                if (args.command == "verify") {
                    const auto verified = ValidateArchive(path, open, job);
                    append({{"package", Detail::PathUtf8(path.filename())}, {"entries", verified.entries}, {"decoded_bytes", verified.decodedBytes}, {"key_digest_mismatches", verified.keyDigestMismatches}, {"sha256", verified.packageSha256}});
                } else {
                    const auto archive = OpenArchive(path, open, job);
                    const auto selected = Select(archive, args);
                    if (args.command == "list") {
                        Json entries = Json::array();
                        for (const auto id : selected) { const auto& e = archive.Entry(id); entries.push_back({{"id", id}, {"path", e.path}, {"size", e.size}, {"stored_size", e.storedSize}, {"flags", e.flags}, {"offset", e.offset}, {"cipher_key", Hex(e.cipherKey)}}); }
                        append({{"package", archive.packageName}, {"entries", entries}});
                    } else if (args.Has("--target")) {
                        if (selected.size() != 1) throw ArchiveError(ErrorCode::Conflict, "指定单文件目标必须准确选中一个条目");
                        const auto digest = ExportEntry(archive, selected.front(), Detail::Utf8Path(args.Get("--target")), args.Has("--overwrite"), job);
                        append({{"package", archive.packageName}, {"path", archive.Entry(selected.front()).path}, {"sha256", digest.sha256}});
                    } else {
                        const auto root = Detail::Utf8Path(args.Get("--output")) / path.stem();
                        const auto extracted = ExtractEntries(archive, selected, root, {!args.Has("--overwrite") ? false : true, !args.Has("--no-manifest")}, job);
                        append({{"package", archive.packageName}, {"workspace", Detail::PathUtf8(root)}, {"items", Results(extracted)}});
                        if (!extracted.Succeeded()) throw ArchiveError(job.cancelled->load() ? ErrorCode::Cancelled : ErrorCode::Io, "部分条目未完成，详见结果清单");
                    }
                }
            });
        } else if (args.command == "pack") {
            if (!args.Has("--output") && !args.Has("--dry-run")) throw ArchiveError(ErrorCode::Conflict, "封包需要 --output");
            if (args.Has("--from-dir")) {
                if (!args.Has("--namespace") || !args.Has("--name") || !args.Has("--workspace") || !args.inputs.empty())
                    throw ArchiveError(ErrorCode::Conflict, "新建需要 --namespace、--name、--workspace，且不同时指定已有工作区");
                ArchiveWorkspace::FromDirectory(Detail::Utf8Path(args.Get("--from-dir")), args.Get("--namespace"), args.Get("--name"),
                    Detail::Utf8Path(args.Get("--workspace")), Number(args.Get("--mode", "3")), job);
                args.inputs.push_back(args.Get("--workspace"));
            }
            if (args.inputs.empty() || (args.Has("--name") && args.inputs.size() > 1)) throw ArchiveError(ErrorCode::Conflict, "指定一个或多个工作区；--name 只适用于单个工作区");
            std::set<std::string> outputNames;
            std::vector<NamedResourcePath> mountedPaths;
            std::set<std::string> plannedDeletes, plannedUndos;
            for (const auto& path : args.Values("--delete")) plannedDeletes.insert(NormalizePath(path));
            for (const auto& path : args.Values("--undo")) plannedUndos.insert(NormalizePath(path));
            for (const auto& input : args.inputs) {
                std::string name;
                try {
                    const auto workspace = ArchiveWorkspace::Load(Detail::Utf8Path(input));
                    name = args.Get("--name", workspace.manifest.packageName);
                    for (const auto& entry : workspace.manifest.entries) {
                        const auto path = NormalizePath(entry.path);
                        if (entry.change == ChangeKind::Add && plannedUndos.contains(path)) continue;
                        if (!plannedDeletes.contains(path) && (entry.change != ChangeKind::Delete || plannedUndos.contains(path))) mountedPaths.push_back({input, entry.path});
                    }
                    for (const auto& assignment : args.Values("--add")) if (const auto split = assignment.find('='); split != std::string::npos)
                        mountedPaths.push_back({input, assignment.substr(0, split)});
                }
                catch (const std::exception&) { continue; } // Bad workspaces fail independently in their task.
                std::transform(name.begin(), name.end(), name.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
                if (!outputNames.insert(name).second) throw ArchiveError(ErrorCode::Conflict, "多个工作区会生成同名包，请分开处理：" + name);
            }
            auto crossConflicts = std::make_shared<std::map<std::string, std::vector<std::string>>>();
            for (const auto& collision : FindResourcePathConflicts(mountedPaths, job)) for (const auto& owner : collision.owners)
                (*crossConflicts)[owner].push_back("跨包重复资源：" + collision.lookupPath);
            for (const auto& input : args.inputs) tasks.emplace_back(input, [&, input, crossConflicts] {
                auto workspace = ArchiveWorkspace::Load(Detail::Utf8Path(input));
                ApplyEdits(workspace, args, job);
                auto changes = workspace.CheckChanges(job);
                if (const auto found = crossConflicts->find(input); found != crossConflicts->end())
                    changes.conflicts.insert(changes.conflicts.end(), found->second.begin(), found->second.end());
                Json record = {{"workspace", input}, {"added", changes.added}, {"replaced", changes.replaced}, {"deleted", changes.deleted}, {"conflicts", changes.conflicts}};
                if (!changes.Valid()) { append(record); throw ArchiveError(ErrorCode::Conflict, changes.conflicts.front()); }
                if (!args.Has("--dry-run")) {
                    const auto name = args.Get("--name", workspace.manifest.packageName);
                    const auto output = Detail::Utf8Path(args.Get("--output")) / Detail::Utf8Path(name);
                    const auto built = BuildArchive(workspace, output, {args.Has("--overwrite"), name}, job);
                    record["output"] = Detail::PathUtf8(output); record["sha256"] = built.packageSha256; record["entries"] = built.entries;
                }
                append(record);
            });
        } else throw ArchiveError(ErrorCode::Conflict, "未知命令：" + args.command);
        const auto results = RunArchiveJobs(tasks, job, concurrency);
        report["tasks"] = Results(results);
        exitCode = job.cancelled->load() ? 130 : (results.Succeeded() ? 0 : 1);
    } catch (const ArchiveError& error) { report["error"] = error.what(); exitCode = error.Code() == ErrorCode::Cancelled ? 130 : 2; }
      catch (const std::exception& error) { report["error"] = error.what(); exitCode = 1; }
    report["exit_code"] = exitCode;
    try { if (args.Has("--report")) Detail::WriteTextAtomic(Detail::Utf8Path(args.Get("--report")), report.dump(2) + "\n"); }
    catch (const std::exception& error) { std::cerr << error.what() << "\n"; exitCode = 1; report["exit_code"] = exitCode; report["report_error"] = error.what(); }
    std::cout << report.dump(2) << "\n";
    cancellation = nullptr;
    return exitCode;
}
