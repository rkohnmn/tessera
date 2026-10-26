// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/harness.hpp
//  A ~100-line test harness. No dependency, no build-system integration
//  beyond the Makefile: `make test` compiles every tests/*.cpp and runs them.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace testing {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct Failure {
    std::string message;
};

/// Scratch path in the system temp directory. Tests must not depend on being
/// run from the repository root, because ctest runs them from the build tree.
inline std::string temp_path(std::string_view name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tessera-tests";
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    return (dir / std::string(name)).string();
}

inline int& failures() {
    static int count = 0;
    return count;
}

inline std::string describe(const std::string& v) { return "\"" + v + "\""; }
inline std::string describe(const char* v) { return std::string("\"") + v + "\""; }
inline std::string describe(bool v) { return v ? "true" : "false"; }
template <typename T>
std::string describe(const T& v) {
    std::ostringstream out;
    out << v;
    return out.str();
}

} // namespace testing

#define TEST(name)                                                                       \
    static void name();                                                                  \
    static ::testing::Registrar registrar_##name(#name, name);                           \
    static void name()

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            throw ::testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": CHECK(" #condition ") failed"};                   \
        }                                                                                \
    } while (false)

#define CHECK_EQ(actual, expected)                                                       \
    do {                                                                                 \
        const auto& a_ = (actual);                                                       \
        const auto& e_ = (expected);                                                     \
        if (!(a_ == e_)) {                                                               \
            throw ::testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": expected " + ::testing::describe(e_) + ", got " +   \
                                     ::testing::describe(a_)};                            \
        }                                                                                \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                          \
    do {                                                                                 \
        const double a_ = (actual);                                                      \
        const double e_ = (expected);                                                    \
        if (std::fabs(a_ - e_) > (tolerance)) {                                          \
            throw ::testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": expected " + ::testing::describe(e_) + " ± " +    \
                                     ::testing::describe(tolerance) + ", got " +          \
                                     ::testing::describe(a_)};                            \
        }                                                                                \
    } while (false)

#define CHECK_THROWS(expression)                                                         \
    do {                                                                                 \
        bool caught_ = false;                                                            \
        try {                                                                            \
            (void)(expression);                                                          \
        } catch (...) {                                                                  \
            caught_ = true;                                                              \
        }                                                                                \
        if (!caught_) {                                                                  \
            throw ::testing::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                                     ": expected an exception from " #expression};        \
        }                                                                                \
    } while (false)
