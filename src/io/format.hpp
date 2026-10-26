// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · format.hpp
//  Terminal rendering for the CLI: aligned result tables, `info` summaries and
//  the column profile report. Kept separate from the TUI so both can share the
//  sparkline/percentile presentation without sharing a renderer.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/dataset.hpp"
#include "core/profile.hpp"
#include "query/exec.hpp"

#include <string>
#include <vector>

namespace tess::io {

struct TableOptions {
    bool unicode = true;
    bool color = false;
    bool header = true;
    std::size_t max_rows = 0;    ///< 0 = every row
    std::size_t max_cell_width = 48;
    int width = 0;               ///< 0 = detect from the terminal
};

/// Aligned box table for query results.
[[nodiscard]] std::string render_table(const query::ResultSet& result, const TableOptions& options);

/// `tessera info` body.
[[nodiscard]] std::string render_info(const Dataset& dataset, const TableOptions& options);

/// `tessera stats` body: one block per column.
[[nodiscard]] std::string render_profiles(const std::vector<ColumnProfile>& profiles, const TableOptions& options,
                                          std::uint64_t total_rows);

/// Eight-step sparkline for a histogram, normalised to its own maximum.
[[nodiscard]] std::string sparkline(const std::vector<Bucket>& buckets, std::size_t width);

/// Extracts a compact, coloured type label such as "integer".
[[nodiscard]] std::string type_label(ColumnType type);

// ── Terminal capability probes (implemented per platform) ───────────────────
[[nodiscard]] bool stdout_is_tty() noexcept;
[[nodiscard]] int terminal_width() noexcept;
[[nodiscard]] bool colors_enabled() noexcept;

} // namespace tess::io
