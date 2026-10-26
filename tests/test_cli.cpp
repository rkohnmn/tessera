// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_cli.cpp
//  Exit codes and stderr text for the argument layer.
//
//  This file exists because of one specific bug: `parse_options` used to be
//  called outside the try/catch in `cli::run`, so a typo in a flag — the single
//  most common thing a new user does — escaped to std::terminate and aborted
//  the process (exit 134 on Linux, exit 3 on Windows) with a C++ runtime
//  message instead of one clean line. Every argument-level failure is pinned
//  here, together with the message it must print, so the try block cannot be
//  narrowed again without a test going red.
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "cli/commands.hpp"

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <io.h>
#  define TESS_DUP _dup
#  define TESS_DUP2 _dup2
#  define TESS_FILENO _fileno
#else
#  include <unistd.h>
#  define TESS_DUP dup
#  define TESS_DUP2 dup2
#  define TESS_FILENO fileno
#endif

namespace {

struct RunResult {
    int code;
    std::string stderr_text;
};

/// Runs the CLI with `stderr` redirected into a scratch file, so argument
/// errors can be asserted on without drowning the test output in them.
RunResult run_cli(const std::vector<std::string>& args) {
    const std::string log = testing::temp_path("cli_stderr.log");
    std::fflush(stderr);
    const int saved = TESS_DUP(TESS_FILENO(stderr));

    std::FILE* sink = std::freopen(log.c_str(), "w", stderr);
    RunResult result{0, {}};
    if (sink != nullptr) {
        result.code = tess::cli::run(args);
        std::fflush(stderr);
    } else {
        result.code = tess::cli::run(args);
    }
    if (saved >= 0) {
        TESS_DUP2(saved, TESS_FILENO(stderr));
#if defined(_WIN32)
        _close(saved);
#else
        close(saved);
#endif
    }

    const std::string text = testing::temp_path("cli_stderr.log");
    std::FILE* file = std::fopen(text.c_str(), "rb");
    if (file != nullptr) {
        std::string buffer(4096, '\0');
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        buffer.resize(read);
        result.stderr_text = buffer;
        std::fclose(file);
    }
    return result;
}

} // namespace

// ── Argument errors: one line on stderr, exit 1, no abort ───────────────────

TEST(cli_unknown_flag_is_an_error_not_a_crash) {
    const RunResult result = run_cli({"--bogusflag", "in.csv"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("unknown option '--bogusflag'") != std::string::npos);
    CHECK(result.stderr_text.find("tessera help") != std::string::npos);
    CHECK(result.stderr_text.find("terminate") == std::string::npos);
    CHECK(result.stderr_text.find("libc++abi") == std::string::npos);
}

TEST(cli_unknown_short_flag_is_an_error) {
    const RunResult result = run_cli({"-Z", "in.csv"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("unknown option '-Z'") != std::string::npos);
}

TEST(cli_non_numeric_row_count_is_an_error) {
    const RunResult result = run_cli({"head", "in.csv", "-n", "many"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("non-negative whole number") != std::string::npos);
    CHECK(result.stderr_text.find("'many'") != std::string::npos);
}

TEST(cli_negative_row_count_is_an_error) {
    const RunResult result = run_cli({"head", "in.csv", "-n", "-4"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("non-negative whole number") != std::string::npos);
}

TEST(cli_flag_missing_its_value_is_an_error) {
    const RunResult result = run_cli({"query", "in.csv", "-q"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("requires a value") != std::string::npos);
}

TEST(cli_multi_character_delimiter_is_an_error) {
    const RunResult result = run_cli({"head", "in.csv", "-d", ";;"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("--delimiter expects a single character") != std::string::npos);
}

TEST(cli_bad_thread_count_is_an_error) {
    const RunResult result = run_cli({"info", "in.csv", "--threads", "lots"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("non-negative whole number") != std::string::npos);
}

// ── Runtime errors keep their exit code and wording ────────────────────────

TEST(cli_unknown_command_is_an_error) {
    const RunResult result = run_cli({"frobnicate", "in.csv"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("unknown command or file: 'frobnicate'") != std::string::npos);
}

TEST(cli_missing_input_file_is_an_error) {
    const RunResult result = run_cli({"info", testing::temp_path("definitely-absent.csv")});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("no such file") != std::string::npos);
}

TEST(cli_error_lines_are_prefixed_with_error) {
    const RunResult result = run_cli({"--nope"});
    CHECK_EQ(result.code, 1);
    CHECK(result.stderr_text.find("error ") != std::string::npos);
}

// ── Success paths return 0 ─────────────────────────────────────────────────

TEST(cli_version_flag_returns_zero) {
    const RunResult result = run_cli({"--version"});
    CHECK_EQ(result.code, 0);
    CHECK(result.stderr_text.empty());
}

TEST(cli_no_arguments_prints_usage_and_returns_zero) {
    const RunResult result = run_cli({});
    CHECK_EQ(result.code, 0);
    CHECK(result.stderr_text.empty());
}

TEST(cli_explicit_help_returns_zero) {
    const RunResult result = run_cli({"help"});
    CHECK_EQ(result.code, 0);
    CHECK(result.stderr_text.empty());
}
