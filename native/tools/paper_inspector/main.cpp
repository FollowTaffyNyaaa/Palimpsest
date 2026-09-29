#include <palimpsest/inspector.h>

#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

int run_inspector(const std::filesystem::path& input, const std::filesystem::path& output) {
    const auto result = palimpsest::inspect_pdf(input, output);
    if (!result.ok) {
        std::cerr << "Palimpsest Paper Inspector failed: " << result.message << '\n';
        return 1;
    }
    std::cout << result.message << '\n';
#if defined(_WIN32)
    std::wcout << L"Output: " << result.output_dir.native() << L'\n';
#else
    std::cout << "Output: " << result.output_dir.string() << '\n';
#endif
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    int wide_argc = 0;
    auto wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (wide_argv == nullptr) {
        std::cerr << "Unable to read the Windows Unicode command line.\n";
        return 2;
    }
    if (wide_argc < 2 || wide_argc > 3) {
        LocalFree(wide_argv);
        std::cerr << "Usage: paper_inspector <paper.pdf> [output_dir]\n";
        return 2;
    }
    const std::filesystem::path input(wide_argv[1]);
    const auto output = wide_argc == 3 ? std::filesystem::path(wide_argv[2]) : (input.parent_path() / L"debug");
    LocalFree(wide_argv);
    return run_inspector(input, output);
#else
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: paper_inspector <paper.pdf> [output_dir]\n";
        return 2;
    }
    const std::filesystem::path input(argv[1]);
    const auto output = argc == 3 ? std::filesystem::path(argv[2]) : (input.parent_path() / "debug");
    return run_inspector(input, output);
#endif
}
