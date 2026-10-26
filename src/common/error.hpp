// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · error.hpp
//  A single exception type for user-facing failures, plus cooperative
//  cancellation for long-running scans (the TUI must stay responsive while a
//  4 GB filter is being evaluated).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tess {

/// A recoverable, user-facing error. `main` prints `what()` and exits non-zero.
class Error : public std::runtime_error {
public:
    explicit Error(std::string message) : std::runtime_error(std::move(message)) {}
};

/// Thrown by workers when the user aborts an operation (Esc in the TUI).
class Canceled : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "operation cancelled"; }
};

/// Cooperative cancellation flag shared between the UI thread and workers.
class CancelToken {
public:
    void cancel() noexcept { flag_.store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool canceled() const noexcept { return flag_.load(std::memory_order_relaxed); }
    void throw_if_canceled() const {
        if (canceled()) throw Canceled{};
    }

private:
    std::atomic<bool> flag_{false};
};

/// Progress callback: `fraction` is in [0, 1]; `phase` is a short verb phrase.
using ProgressFn = std::function<void(double fraction, std::string_view phase)>;

[[noreturn]] inline void fail(std::string message) { throw Error(std::move(message)); }

} // namespace tess
