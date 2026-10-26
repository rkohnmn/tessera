// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_main.cpp
//  Runs every registered case and prints a TAP-ish summary. Exit code is the
//  number of failures, so `make test` fails loudly in CI.
//
//  Pass one or more substrings to run a subset:
//      ./build/tessera_tests profile query
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool selected(const char* name, const std::vector<std::string_view>& filters) {
    if (filters.empty()) return true;
    const std::string_view candidate{name};
    for (const auto& filter : filters) {
        if (candidate.find(filter) != std::string_view::npos) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> filters;
    for (int i = 1; i < argc; ++i) filters.emplace_back(argv[i]);

    int failed = 0;
    int total = 0;
    int skipped = 0;
    for (const auto& test : testing::registry()) {
        if (!selected(test.name, filters)) {
            ++skipped;
            continue;
        }
        ++total;
        try {
            test.fn();
            std::printf("  ok    %s\n", test.name);
        } catch (const testing::Failure& failure) {
            ++failed;
            std::printf("  FAIL  %s\n        %s\n", test.name, failure.message.c_str());
        } catch (const std::exception& error) {
            ++failed;
            std::printf("  FAIL  %s\n        uncaught exception: %s\n", test.name, error.what());
        } catch (...) {
            ++failed;
            std::printf("  FAIL  %s\n        uncaught non-standard exception\n", test.name);
        }
    }
    if (skipped > 0) {
        std::printf("\n  %d passed, %d failed, %d total (%d filtered out)\n", total - failed, failed, total,
                    skipped);
    } else {
        std::printf("\n  %d passed, %d failed, %d total\n", total - failed, failed, total);
    }
    return failed == 0 ? 0 : 1;
}
