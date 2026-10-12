#include "TestSuite.h"
#include "ArchiveWorkspace.h"
#include "Json.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>

namespace PKNPacker::Tests {
namespace {
struct Suite {
    unsigned passed = 0, failed = 0;
    Json cases = Json::array();
    void Check(bool condition, const std::string& name) {
        condition ? ++passed : ++failed;
        cases.push_back({{"name", name}, {"passed", condition}});
        if (!condition) std::cerr << "FAIL: " << name << "\n";
    }
    template<class Operation> void Reject(Operation operation, const std::string& name) {
        try { operation(); Check(false, name); } catch (const ArchiveError&) { Check(true, name); }
    }
};
void Bytes(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    Detail::AtomicFile transaction(path, true); Detail::File file(transaction.temporary, true); file.Write(data); file.Flush(); file.Close(); transaction.Commit();
}
std::vector<std::uint8_t> Pattern(std::size_t size, unsigned salt = 7) {
    std::vector<std::uint8_t> bytes(size);
    std::uint32_t value = salt;
    for (auto& byte : bytes) { value = value * 1664525U + 1013904223U; byte = static_cast<std::uint8_t>(value >> 24); }
    return bytes;
}
std::string WholeHash(const std::vector<std::uint8_t>& bytes) {
    ContentHash hash(HashAlgorithm::Sha256); hash.Update(bytes); return Hex(hash.Finish());
}
void CipherTests(Suite& suite, const std::filesystem::path& root) {
    const auto fixture = Json::parse(Detail::ReadText(root / "Tests/PKNPacker.Tests/Fixtures/SnowVectors.json"));
    for (const auto& vector : fixture.at("Vectors")) {
        const auto key = Unhex(vector.at("Key").get<std::string>()), expected = Unhex(vector.at("Keystream").get<std::string>());
        SnowCipher cipher(key); std::vector<std::uint8_t> actual(expected.size()); cipher.Transform(actual, true);
        suite.Check(actual == expected, "Original instructions: " + vector.at("Name").get<std::string>());
        const auto plain = Pattern(1024); auto encoded = plain; SnowCipher encrypt(key); encrypt.Transform(encoded, true);
        SnowCipher decrypt(key); decrypt.Transform(std::span(encoded).first(4)); decrypt.Transform(std::span(encoded).subspan(4, 60)); decrypt.Transform(std::span(encoded).subspan(64));
        suite.Check(encoded == plain, "Cipher chunk continuity: " + vector.at("Name").get<std::string>());
    }
    suite.Reject([] { SnowCipher cipher(std::array<std::uint8_t, 1>{}); }, "Short cipher key");
    suite.Reject([] { SnowCipher cipher(std::array<std::uint8_t, 16>{}); auto data = Pattern(5); cipher.Transform(data); }, "Unaligned cipher input");
}
void StorageTests(Suite& suite, const std::filesystem::path& run) {
    const std::vector<std::size_t> sizes{0, 1, 3, 4, 7, 64, 1023, 1024, 1025, 65537};
    const auto input = run / "Input"; Detail::EnsureSafeRoot(input);
    for (const auto size : sizes) Bytes(input / (std::to_string(size) + ".bin"), Pattern(size));
    const std::string unicode = "한국어_中文.txt";
    Bytes(input / Detail::Utf8Path(unicode), Pattern(41));
    for (std::uint32_t mode = 0; mode <= 4; ++mode) {
        const auto name = "Mode" + std::to_string(mode) + "_00000.pkn";
        auto workspace = ArchiveWorkspace::FromDirectory(input, "Assets", name, run / ("Mode" + std::to_string(mode)), mode);
        const auto output = run / "Built" / name;
        const auto report = BuildArchive(workspace, output);
        suite.Check(report.entries == sizes.size() + 1, "New archive mode " + std::to_string(mode));
        const auto archive = OpenArchive(output);
        for (const auto& entry : archive.entries) {
            const auto source = input / Detail::Utf8Path(entry.path.substr(7));
            suite.Check(ReadEntryTo(archive, entry.id, {}).sha256 == HashFile(source).sha256, "Stored content mode " + std::to_string(mode) + ": " + entry.path);
        }
        const auto targetId = std::find_if(workspace.manifest.entries.begin(), workspace.manifest.entries.end(), [](const auto& e) { return e.path == "Assets/7.bin"; })->id;
        const auto small = run / "Small.bin", large = run / "Large.bin";
        Bytes(small, Pattern(2)); Bytes(large, Pattern(100013));
        workspace.Replace(targetId, large); BuildArchive(workspace, run / "Larger" / name);
        const auto larger = OpenArchive(run / "Larger" / name); const auto& largerEntry = *std::find_if(larger.entries.begin(), larger.entries.end(), [](const auto& e) { return e.path == "Assets/7.bin"; });
        suite.Check(largerEntry.size == 100013 && ReadEntryTo(larger, largerEntry.id, {}).sha256 == HashFile(large).sha256, "Growing replacement mode " + std::to_string(mode));
        workspace.Replace(targetId, small); BuildArchive(workspace, run / "Smaller" / name);
        const auto smaller = OpenArchive(run / "Smaller" / name); const auto& smallerEntry = *std::find_if(smaller.entries.begin(), smaller.entries.end(), [](const auto& e) { return e.path == "Assets/7.bin"; });
        suite.Check(smallerEntry.size == 2 && ReadEntryTo(smaller, smallerEntry.id, {}).sha256 == HashFile(small).sha256, "Shrinking replacement mode " + std::to_string(mode));
        suite.Reject([&] { workspace.Add("assets/7.BIN", small); }, "Normalized conflict mode " + std::to_string(mode));
        suite.Reject([&] { BuildArchive(workspace, output); }, "Existing output rejected mode " + std::to_string(mode));
        suite.Check(ReadEntry(archive, largerEntry.id, 1, 0).size() <= 1, "Bounded preview mode " + std::to_string(mode));
        if (mode == 3) {
            const auto resumedRoot = run / "ResumedExtraction";
            suite.Reject([&] { ExtractEntries(archive, {0, 0}, run / "DuplicateSelection", {}); }, "Duplicate extraction IDs rejected");
            suite.Check(ExtractEntries(archive, {0, 1}, resumedRoot, {}).Succeeded(), "Partial extraction succeeds");
            suite.Check(!ArchiveWorkspace::Load(resumedRoot).manifest.complete, "Partial workspace cannot be packed");
            std::vector<EntryId> rest;
            for (const auto& entry : archive.entries) if (entry.id >= 2) rest.push_back(entry.id);
            suite.Check(ExtractEntries(archive, rest, resumedRoot, {}).Succeeded(), "Remaining extraction resumes");
            auto resumed = ArchiveWorkspace::Load(resumedRoot);
            suite.Check(resumed.manifest.complete, "Resumed workspace is complete");
            resumed.Replace(0, large);
            suite.Reject([&] { ExtractEntries(archive, {0}, resumedRoot, {true, true}); }, "Extraction cannot overwrite pending workspace edits");
            auto clone = ArchiveWorkspace::FromArchive(archive, run / "Clone");
            const auto originalHash = HashFile(output).sha256;
            clone.Delete(archive.entries.front().id); clone.Undo(archive.entries.front().id);
            suite.Check(clone.manifest.entries.front().change == ChangeKind::Keep, "Undo delete");
            clone.Replace(archive.entries.front().id, large); clone.Undo(archive.entries.front().id);
            suite.Check(!clone.manifest.entries.front().extracted, "Undo staged replacement");
            clone.Add("Extra/新增.txt", small, 4); const auto addedId = clone.manifest.entries.back().id;
            clone.Delete(archive.entries.front().id);
            const auto edited = run / "Edited/Edited_00000.pkn"; BuildArchive(clone, edited);
            const auto editedArchive = OpenArchive(edited);
            suite.Check(editedArchive.entries.size() == archive.entries.size(), "Explicit add and delete counts");
            suite.Check(std::any_of(editedArchive.entries.begin(), editedArchive.entries.end(), [](const auto& e) { return e.path == "Extra/新增.txt"; }), "Added Unicode resource identity");
            clone.Undo(addedId); suite.Check(clone.manifest.entries.size() == archive.entries.size(), "Undo add");
            suite.Check(HashFile(output).sha256 == originalHash, "Original archive preserved");
            clone.manifest.entries.erase(clone.manifest.entries.begin());
            suite.Reject([&] { BuildArchive(clone, run / "Missing/Missing_00000.pkn"); }, "Implicit deletion rejected");
            ArchiveJob cancel; cancel.progress = [&](const JobProgress& p) { if (p.stage == "生成资源包") cancel.cancelled->store(true); };
            const auto cancelled = run / "Cancelled/Cancelled_00000.pkn";
            suite.Reject([&] { BuildArchive(workspace, cancelled, {}, cancel); }, "Cancelled writer rejected");
            suite.Check(!std::filesystem::exists(cancelled), "Cancelled output never committed");
            bool leaked = false; for (const auto& f : std::filesystem::directory_iterator(cancelled.parent_path())) if (f.path().filename().wstring().find(L"pkntmp") != std::wstring::npos) leaked = true;
            suite.Check(!leaked, "Cancelled temporary output cleaned");
            HANDLE lock = CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            suite.Reject([&] { BuildArchive(workspace, output, {true, {}}); }, "Occupied output rejected"); CloseHandle(lock);
            suite.Check(HashFile(output).sha256 == originalHash, "Occupied output remains intact");
        }
    }
    for (const auto& path : {"../escape.txt", "a/../../escape.txt", "C:/escape.txt", "/absolute", "a:stream", "a//b", "a/CON.txt", "a/b.", "a/b "})
        suite.Reject([&] { NormalizePath(path); }, std::string("Path rejection: ") + path);
    suite.Check(NormalizePath("ABC\\file.TXT") == "abc/file.txt", "Case and separator normalization");
    const std::vector<NamedResourcePath> mounted{{"One", "ABC\\File.TXT"}, {"Two", "abc/file.txt"}, {"Two", "Extra/中文.txt"}};
    const auto conflicts = FindResourcePathConflicts(mounted);
    suite.Check(conflicts.size() == 1 && conflicts.front().lookupPath == "abc/file.txt" && conflicts.front().owners.size() == 2, "Cross-package conflicts use loader path rules");
    const std::vector<NamedResourcePath> uniqueMounted{{"One", "Assets/A"}, {"Two", "Assets/B"}};
    suite.Check(FindResourcePathConflicts(uniqueMounted).empty(), "Independent package paths accepted");
    suite.Reject([] { ValidatePackageName("Name.pkn"); }, "Unnumbered package rejected");
    suite.Reject([] { ValidatePackageName("Name_abc.pkn"); }, "Non-numeric package suffix rejected");
    const auto escaped = run / "Escape"; Detail::EnsureSafeRoot(escaped);
    suite.Reject([&] { Detail::SafeChild(escaped, "../outside"); }, "Extraction traversal rejected");
    const auto link = escaped / "Link", destination = run / "LinkDestination"; Detail::EnsureSafeRoot(destination);
    if (CreateSymbolicLinkW(link.c_str(), destination.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        suite.Reject([&] { Detail::SafeChild(escaped, "Link/payload.bin"); }, "Extraction reparse point rejected");
    } else suite.cases.push_back({{"name", "Extraction reparse point test"}, {"skipped", true}, {"windows_error", GetLastError()}});
}
void MalformedTests(Suite& suite, const std::filesystem::path& run) {
    const auto goodPath = run / "Built/Mode3_00000.pkn"; const auto archive = OpenArchive(goodPath);
    const auto good = Detail::ReadText(goodPath); const auto parameters = Detail::DeriveName("packs", archive.packageName);
    const auto mutate = [&](std::string data, const std::string& kind) {
        const auto path = run / "Malformed" / kind / archive.packageName;
        Bytes(path, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
        suite.Reject([&] { ValidateArchive(path, {}, {}, false); }, kind);
    };
    auto truncated = good; truncated.resize(1024); mutate(truncated, "Aligned truncated data");
    truncated.resize(37); mutate(truncated, "Truncated header");
    auto wrong = good; wrong[static_cast<std::size_t>(parameters.headerOffset)] ^= 1; mutate(wrong, "Header checksum");
    auto count = good;
    auto h = std::span(reinterpret_cast<std::uint8_t*>(count.data() + parameters.headerOffset), 12);
    SnowCipher decode(parameters.headerKey); decode.Transform(h); WriteU32(h, 0xFFFFFFF2U); WriteU32(h.subspan(5), 0xFFFFFFF0U);
    SnowCipher encode(parameters.headerKey); encode.Transform(h, true); mutate(count, "Count overflow with valid header checksum");
    auto version = good; auto v = std::span(reinterpret_cast<std::uint8_t*>(version.data() + parameters.headerOffset), 12);
    SnowCipher dv(parameters.headerKey); dv.Transform(v); v[4] = 7; WriteU32(v, static_cast<std::uint32_t>(archive.entries.size()) + 7);
    SnowCipher ev(parameters.headerKey); ev.Transform(v, true); mutate(version, "Unknown version");
    auto offset = good;
    std::uint64_t indexSize = 0; for (const auto& entry : archive.entries) indexSize += 40 + entry.rawPath.size() * 2;
    auto index = std::span(reinterpret_cast<std::uint8_t*>(offset.data() + parameters.indexOffset), static_cast<std::size_t>(Detail::Align(indexSize, 4)));
    SnowCipher di(parameters.indexKey); di.Transform(index);
    const auto metadata = 4 + archive.entries.front().rawPath.size() * 2;
    const auto oldBlock = ReadU32(index.subspan(metadata + 8));
    WriteU32(index.subspan(metadata + 8), 0xFFFFFFF0U); WriteU32(index.subspan(metadata), ReadU32(index.subspan(metadata)) - oldBlock + 0xFFFFFFF0U);
    SnowCipher ei(parameters.indexKey); ei.Transform(index, true); mutate(offset, "Offset overflow with valid index checksum");
    auto payload = good; const auto entry = std::find_if(archive.entries.begin(), archive.entries.end(), [](const auto& e) { return e.size > 100; });
    payload[static_cast<std::size_t>(entry->offset + 8)] ^= 0x44; mutate(payload, "Compressed payload corruption");
    ArchiveJob job; const auto batch = RunArchiveJobs({{"bad", [] { throw ArchiveError(ErrorCode::Io, "expected failure"); }}, {"good", [] {}}}, job);
    suite.Check(batch.items[0].status == ItemStatus::Failed && batch.items[1].status == ItemStatus::Success, "Batch continues independent task after failure");
}
void SampleTests(Suite& suite, const std::filesystem::path& samples, const std::filesystem::path& run, bool all, Json& sampleReport) {
    if (!std::filesystem::exists(samples)) { sampleReport["skipped"] = "No original Packs directory"; return; }
    std::vector<std::filesystem::path> paths;
    if (all) {
        for (const auto& file : std::filesystem::directory_iterator(samples)) if (file.path().extension() == ".pkn") paths.push_back(file.path());
    } else {
        for (const auto* name : {"Shader_00000.pkn", "Config_00000.pkn", "ModelsChrDefault_00000.pkn", "Effects_00001.pkn", "Images_00001.pkn", "ModelsChrPlBazzi_00003.pkn"}) paths.push_back(samples / name);
    }
    sampleReport["archives"] = Json::array();
    for (const auto& path : paths) {
        try {
            const auto report = ValidateArchive(path);
            sampleReport["archives"].push_back({{"name", Detail::PathUtf8(path.filename())}, {"entries", report.entries}, {"decoded_bytes", report.decodedBytes}, {"key_digest_mismatches", report.keyDigestMismatches}, {"sha256", report.packageSha256}});
            suite.Check(true, "Original archive: " + Detail::PathUtf8(path.filename()));
        } catch (const std::exception& error) { suite.Check(false, "Original archive: " + Detail::PathUtf8(path.filename())); sampleReport["archives"].push_back({{"name", Detail::PathUtf8(path.filename())}, {"error", error.what()}}); }
    }
    const auto shader = OpenArchive(samples / "Shader_00000.pkn");
    suite.Reject([&] { Detail::AtomicFile blocked(samples / "Shader_00000.pkn", true); }, "Workspace original package output guard");
    auto caseAlias = (samples / "Shader_00000.pkn").wstring();
    for (auto& c : caseAlias) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
    suite.Reject([&] { Detail::AtomicFile blocked(std::filesystem::path(caseAlias), true); }, "Original output guard rejects case aliases");
    std::vector<EntryId> ids{0}; const auto extraction = ExtractEntries(shader, ids, run / "ShaderWorkspace");
    suite.Check(extraction.Succeeded(), "Original full extraction and manifest");
    auto workspace = ArchiveWorkspace::Load(run / "ShaderWorkspace");
    const auto output = run / "RoundTrip/Shader_00000.pkn"; BuildArchive(workspace, output);
    const auto rebuilt = OpenArchive(output);
    suite.Check(ReadEntryTo(shader, 0, {}).sha256 == ReadEntryTo(rebuilt, 0, {}).sha256, "Original Shader round trip SHA-256");
    sampleReport["shader_rebuilt_sha256"] = HashFile(output).sha256;
    BuildArchive(workspace, run / "Renamed/Alternative_00012.pkn");
    const auto renamed = OpenArchive(run / "Renamed/Alternative_00012.pkn");
    suite.Check(ReadEntryTo(renamed, 0, {}).sha256 == ReadEntryTo(shader, 0, {}).sha256, "Renamed package re-encrypts header and index");
    suite.Reject([&] { OpenArchive(run / "Renamed/Alternative_00012.pkn", {"packs", "Shader_00000.pkn"}); }, "Wrong logical archive identity rejected");
    std::filesystem::remove(workspace.InputFile(workspace.manifest.entries.front()));
    suite.Reject([&] { BuildArchive(workspace, run / "MissingExtracted/MissingExtracted_00000.pkn"); }, "Missing extracted file requires explicit deletion");
}
}
int Run(const std::filesystem::path& root, const std::filesystem::path& samples, bool all) {
    Suite suite; Json sampleReport;
    const auto run = root / "Artifacts/TestRuns" / std::to_string(GetCurrentProcessId()); Detail::EnsureSafeRoot(run);
    try { CipherTests(suite, root); StorageTests(suite, run); MalformedTests(suite, run); SampleTests(suite, samples, run, all, sampleReport); }
    catch (const std::exception& error) { ++suite.failed; suite.cases.push_back({{"unexpected_error", error.what()}}); std::cerr << error.what() << "\n"; }
    const Json report = {{"passed", suite.passed}, {"failed", suite.failed}, {"cases", suite.cases}, {"samples", sampleReport}};
    Detail::WriteTextAtomic(run / "TestReport.json", report.dump(2) + "\n");
    std::cout << "Passed: " << suite.passed << ", failed: " << suite.failed << "\nReport: " << Detail::PathUtf8(run / "TestReport.json") << "\n";
    return suite.failed ? 1 : 0;
}
}
