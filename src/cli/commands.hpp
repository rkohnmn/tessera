// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · commands.hpp
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

namespace tess::cli {

/// Entry point for `tessera ...`. Returns a process exit code.
int run(const std::vector<std::string>& args);

/// One-line version banner, shared by `--version` and the TUI help screen.
[[nodiscard]] std::string version_banner();

} // namespace tess::cli
