#include "TestSuite.h"
#include "ArchiveCatalog.h"
#include <Windows.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::wstring module(32768, L'\0'); module.resize(GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())));
    auto root = std::filesystem::path(module).parent_path();
    while (root != root.parent_path() && !std::filesystem::exists(root / "Tests/PKNPacker.Tests/Fixtures/SnowVectors.json")) root = root.parent_path();
    auto samples = root.parent_path() / "Packs";
    bool all = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option(argv[i]);
        if (option == L"--all") all = true;
        else if (option == L"--root" && i + 1 < argc) root = argv[++i];
        else if (option == L"--samples" && i + 1 < argc) samples = argv[++i];
        else { std::cerr << "PKNPacker.Tests [--root 工程目录] [--samples Packs目录] [--all]\n"; return 2; }
    }
    return PKNPacker::Tests::Run(root, samples, all);
}
