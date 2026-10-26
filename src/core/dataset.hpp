// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · dataset.hpp
//  The in-memory model of a delimited file: row offsets, column metadata and
//  O(1) cell access over the mapped bytes. Nothing is copied out of the file
//  except when the source is UTF-16 or JSON Lines.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "common/error.hpp"
#include "core/dialect.hpp"
#include "core/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tess {

enum class ColumnType : std::uint8_t { Unknown, Integer, Float, Boolean, Timestamp, Text };

[[nodiscard]] std::string_view to_string(ColumnType type);
[[nodiscard]] bool is_numeric(ColumnType type) noexcept;
[[nodiscard]] char type_glyph(ColumnType type) noexcept;

struct ColumnMeta {
    std::string name;
    ColumnType type = ColumnType::Text;
    /// Share of sampled non-empty values that parse as `type` (1.0 = clean).
    double type_confidence = 0.0;
    /// Widest sampled value in display columns; used to size the grid.
    std::uint32_t sample_width = 8;
    std::uint32_t sample_nulls = 0;
};

struct OpenOptions {
    bool use_sidecar = true;
    bool write_sidecar = false;
    std::optional<char> delimiter;
    std::optional<bool> header;
    bool quotes = true;
    unsigned threads = 0; ///< 0 = hardware concurrency
    CancelToken* cancel = nullptr;
    ProgressFn progress;
    std::size_t infer_rows = 4096;
};

struct DatasetInfo {
    std::string path;
    std::string name;
    Format format = Format::Delimited;
    Encoding encoding = Encoding::Utf8;
    char delimiter = ',';
    char quote = '"';
    bool has_header = true;
    bool crlf = false;
    std::uint64_t bytes = 0;
    std::uint64_t rows = 0;
    std::uint32_t columns = 0;
    double open_seconds = 0.0;
    double index_seconds = 0.0;
    std::uint64_t index_bytes = 0;
    bool index_from_sidecar = false;
    std::uint64_t ragged_rows = 0;
    std::uint64_t widest_row_bytes = 0;
    std::uint64_t null_cells_sampled = 0;
    std::vector<std::string> notes;
};

class Dataset {
public:
    /// Opens `path`, sniffs the dialect and builds the row index.
    [[nodiscard]] static std::shared_ptr<Dataset> open(std::string path, const OpenOptions& options = {});
    /// Builds a dataset from an in-memory document (tests, stdin, JSONL).
    [[nodiscard]] static std::shared_ptr<Dataset> parse(std::string text, std::string name = "<memory>",
                                                        const OpenOptions& options = {});

    Dataset(const Dataset&) = delete;
    Dataset& operator=(const Dataset&) = delete;

    // ── Identity ─────────────────────────────────────────────────────────────
    [[nodiscard]] const Source& source() const noexcept { return *source_; }
    [[nodiscard]] const Dialect& dialect() const noexcept { return dialect_; }
    [[nodiscard]] const DatasetInfo& info() const noexcept { return info_; }
    [[nodiscard]] const std::vector<ColumnMeta>& columns() const noexcept { return columns_; }
    [[nodiscard]] const std::vector<std::uint64_t>& row_offsets() const noexcept { return starts_; }

    [[nodiscard]] std::uint64_t row_count() const noexcept { return data_rows_; }
    [[nodiscard]] std::uint32_t column_count() const noexcept { return static_cast<std::uint32_t>(columns_.size()); }
    [[nodiscard]] std::string_view column_name(std::uint32_t index) const noexcept;
    [[nodiscard]] ColumnType column_type(std::uint32_t index) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> column_index(std::string_view name) const noexcept;

    // ── Rows and cells ───────────────────────────────────────────────────────
    /// Full record text for a data row (no terminator, quotes preserved).
    [[nodiscard]] std::string_view row_text(std::uint64_t row) const;
    /// Byte offset of a data row inside the source.
    [[nodiscard]] std::uint64_t row_offset(std::uint64_t row) const;
    /// Cell text: outer quotes removed, interior bytes verbatim (doubled quotes
    /// are preserved so CSV export is byte-exact; use decode_cell for display).
    [[nodiscard]] std::string_view cell(std::uint64_t row, std::uint32_t column) const;
    /// Cell text with `""` collapsed to `"`. Allocates only for quoted values.
    [[nodiscard]] std::string decode_cell(std::uint64_t row, std::uint32_t column) const;
    /// Splits a data row into field spans (quote aware). Reuses `scratch`.
    void row_fields(std::uint64_t row, std::vector<FieldSpan>& scratch) const;
    [[nodiscard]] std::string_view field_text(std::string_view row, const FieldSpan& span) const;

    /// Typed accessors. Return nullopt when the cell is empty or unparseable.
    [[nodiscard]] std::optional<std::int64_t> cell_int(std::uint64_t row, std::uint32_t column) const;
    [[nodiscard]] std::optional<double> cell_double(std::uint64_t row, std::uint32_t column) const;

    /// Header hash used to validate cached indexes.
    [[nodiscard]] std::uint64_t header_signature() const noexcept { return header_signature_; }

    /// Writes a `.tessera.idx` sidecar next to the file. Returns its size.
    std::uint64_t save_index_cache() const;

private:
    Dataset() = default;

    void initialise(const OpenOptions& options, double open_start);
    void read_header(const OpenOptions& options);
    void infer_types(const OpenOptions& options);

    std::shared_ptr<Source> source_;
    Dialect dialect_;
    DatasetInfo info_;
    std::vector<std::uint64_t> starts_; ///< Start offset of every physical record.
    std::vector<ColumnMeta> columns_;
    std::uint64_t data_rows_ = 0;
    std::uint64_t first_data_row_ = 0;
    std::uint64_t header_signature_ = 0;
    bool materialized_ = false;
};

/// Materialises JSON Lines into a CSV buffer (one object per line, scalars only
/// are unescaped; nested values are preserved as compact JSON text).
[[nodiscard]] std::string jsonl_to_csv(std::string_view jsonl, std::vector<std::string>& columns_out,
                                      std::uint64_t* skipped = nullptr);

} // namespace tess
