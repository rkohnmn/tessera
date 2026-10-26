// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · main.cpp
//  Argument intake only. On Windows the command line is read as UTF-16 and
//  converted to UTF-8 so non-ASCII paths work, which plain `argv` cannot do.
// ─────────────────────────────────────────────────────────────────────────────
#include "cli/commands.hpp"

#include <string>
#include <vector>

#if defined(TESSERA_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

#if defined(TESSERA_WINDOWS)
static std::vector<std::string> collect_arguments() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) return args;
    for (int i = 1; i < count; ++i) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1) {
            args.emplace_back();
            continue;
        }
        std::string utf8(static_cast<std::size_t>(needed - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8.data(), needed, nullptr, nullptr);
        args.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return args;
}
#endif

#if defined(TESSERA_WINDOWS)
int wmain() {
    return tess::cli::run(collect_arguments());
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return tess::cli::run(args);
}
#endif
