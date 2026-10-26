// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · dialect.hpp
//  Format detection: encoding, delimiter, quoting, header and record format.
//  Everything here is heuristic but explainable — each decision is recorded in
//  `Dialect::notes` so `tessera info` can tell the user *why* it chose a
//  delimiter, which is exactly the question people ask when a file renders wrong.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tess {

enum class Format { Delimited, JsonLines };

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Latin1 };

[[nodiscard]] std::string_view to_string(Format format);
[[nodiscard]] std::string_view to_string(Encoding encoding);

struct Dialect {
    Format format = Format::Delimited;
    Encoding encoding = Encoding::Utf8;
    char delimiter = ',';
    char quote = '"';
    bool has_header = true;
    bool crlf = false;
    bool quotes_present = false;
    std::uint64_t bom_bytes = 0;
    /// Set when the file was transcoded to UTF-8 so offsets refer to the copy.
    bool transcoded = false;
    /// Human-readable explanation of the auto-detection, shown by `info`.
    std::vector<std::string> notes;

    [[nodiscard]] std::string delimiter_name() const;
};

struct SniffOptions {
    std::optional<char> force_delimiter;
    std::optional<bool> force_header;
    bool allow_quotes = true;
    /// Extension hint, e.g. ".tsv" or ".jsonl".
    std::string extension;
};

struct SniffResult {
    Dialect dialect;
    /// Number of fields on the first record (0 when unknown).
    std::uint32_t first_record_fields = 0;
    bool json_array = false; ///< A .json file that is a top-level array.
};

/// Inspects up to ~1 MB of `data` (already UTF-8) and returns a dialect.
[[nodiscard]] SniffResult sniff(const char* data, std::uint64_t size, const SniffOptions& options);

/// Detects UTF-16/Latin-1 content that must be converted before scanning.
[[nodiscard]] Encoding detect_encoding(const char* data, std::uint64_t size);

/// Converts UTF-16LE/BE (with optional BOM) to UTF-8. Returns the byte offset
/// of the first character after the BOM in the *source* buffer.
[[nodiscard]] std::string utf16_to_utf8(std::string_view bytes, bool little_endian, std::uint64_t& source_bom_bytes);

/// Splits one record into field bounds (offset + length, quotes stripped).
/// `row` must exclude its terminating newline. This is the single hot loop that
/// every view, query and export path goes through.
struct FieldSpan {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    bool quoted = false;
};

void split_fields(std::string_view row, char delimiter, char quote, std::vector<FieldSpan>& out);

/// Returns the span of field `index` without materialising the whole record.
/// Returns false when the record has fewer fields.
[[nodiscard]] bool field_at(std::string_view row, char delimiter, char quote,
                            std::uint32_t index, FieldSpan& out);

/// Counts fields in a record (quote-aware).
[[nodiscard]] std::uint32_t count_fields(std::string_view row, char delimiter, char quote) noexcept;

/// Replaces doubled quote characters inside a quoted field with single ones.
[[nodiscard]] std::string decode_field(std::string_view field, bool quoted, char quote);

/// Appends `value` to `out` using RFC 4180 quoting with the given delimiter.
void append_csv_field(std::string& out, std::string_view value, char delimiter, char quote);

/// Appends a JSON string literal (with escaping) for `value`.
void append_json_string(std::string& out, std::string_view value);

} // namespace tess
