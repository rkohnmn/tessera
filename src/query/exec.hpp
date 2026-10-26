// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · exec.hpp
//  Streaming execution over the row index. Filters are evaluated directly
//  against mapped cell bytes; only rows that survive a WHERE clause are ever
//  materialised, and a LIMIT lets the scan stop early.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "query/parser.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tess::query {

struct RunOptions {
    CancelToken* cancel = nullptr;
    ProgressFn progress;
    /// 0 = unlimited. Applies to produced rows, not scanned rows.
    std::uint64_t max_rows = 0;
};

struct ResultSet {
    std::vector<std::string> headers;
    std::vector<ColumnType> types;
    std::vector<std::vector<std::string>> rows;
    std::uint64_t scanned = 0;
    std::uint64_t matched = 0;
    std::uint64_t groups = 0;
    double seconds = 0.0;
    bool truncated = false;
};

/// Compares two values with SQL-ish semantics. Null compares greater than
/// everything so ascending sorts keep nulls at the bottom.
[[nodiscard]] int compare_values(const Value& a, const Value& b) noexcept;
/// Case-insensitive SQL LIKE with '%' and '_' wildcards and '\' escapes.
[[nodiscard]] bool like_match(std::string_view text, std::string_view pattern) noexcept;
/// Output column type, used for alignment and for CSV consumers.
[[nodiscard]] ColumnType infer_output_type(const Expr& expr, const Dataset& dataset);

/// Full statement execution (aggregation, sorting, projection, limit).
[[nodiscard]] ResultSet execute(const Dataset& dataset, const Query& query, const RunOptions& options);

/// Evaluates a boolean predicate over every row and returns matching row ids
/// in file order. This is what the TUI filter bar uses.
[[nodiscard]] std::vector<std::uint64_t> filter_rows(const Dataset& dataset, const Expr& predicate,
                                                     const RunOptions& options, double* seconds = nullptr);

/// Case-insensitive substring search across the given columns (all when empty).
/// Used when a typed filter is not a valid expression.
[[nodiscard]] std::vector<std::uint64_t> search_rows(const Dataset& dataset, std::string_view needle,
                                                     const std::vector<std::uint32_t>& columns,
                                                     const RunOptions& options, double* seconds = nullptr);

/// Stable multi-key sort of row ids, in place.
void sort_rows(const Dataset& dataset, std::vector<std::uint64_t>& rows, const std::vector<OrderKey>& keys,
               const RunOptions& options);

/// Every row id, in file order (allocation only, no scanning).
[[nodiscard]] std::vector<std::uint64_t> all_rows(const Dataset& dataset);

} // namespace tess::query
