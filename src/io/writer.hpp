// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · writer.hpp
//  Streaming export. Writes in megabyte-sized chunks straight from the mapped
//  file, so exporting a 40 GB CSV costs a bounded amount of memory and can be
//  cancelled mid-flight without leaving a half-written buffer in RAM.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/dataset.hpp"
#include "query/exec.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace tess::io {

enum class OutputFormat { Csv, Tsv, JsonLines };

[[nodiscard]] OutputFormat parse_output_format(std::string_view name);
[[nodiscard]] std::string to_string(OutputFormat format);

struct ExportStats {
    std::uint64_t rows = 0;
    std::uint64_t bytes = 0;
    double seconds = 0.0;
};

/// Exports `rows` (nullptr = every row) to `path` ("-" = stdout).
/// Preserves the original quoting exactly for CSV/TSV output.
[[nodiscard]] ExportStats export_rows(const Dataset& dataset, const std::vector<std::uint64_t>* rows,
                                      const std::vector<std::uint32_t>& columns, const std::string& path,
                                      OutputFormat format, const query::RunOptions& options);

/// Serialises an already-materialised result set (used by `tessera query`).
void write_result(std::ostream& out, const query::ResultSet& result, OutputFormat format);

} // namespace tess::io
