// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · dialect.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/dialect.hpp"

#include "common/util.hpp"
#include "core/simd.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace tess {
namespace {

constexpr std::uint64_t kSniffBytes = 1u << 20; // 1 MiB is plenty for structure
constexpr std::array<char, 4> kCandidateDelimiters = {',', '\t', ';', '|'};

struct RecordView {
    std::uint64_t offset;
    std::uint64_t length;
};

/// Splits `data` into up to `max_records` logical records (quote aware).
std::vector<RecordView> sample_records(std::string_view data, char quote, std::size_t max_records) {
    std::vector<RecordView> records;
    std::uint64_t start = 0;
    bool in_quotes = false;
    const char* const base = data.data();
    const char* const end = base + data.size();
    const char* p = base;

    while (p < end && records.size() < max_records) {
        if (quote != 0) {
            p = swar::find_either(p, end, '\n', quote);
            if (p == end) break;
            if (*p == quote) {
                in_quotes = !in_quotes;
                ++p;
                continue;
            }
        } else {
            p = swar::find_byte(p, end, '\n');
            if (p == end) break;
        }
        if (!in_quotes) {
            records.push_back({start, static_cast<std::uint64_t>(p - base) - start});
            start = static_cast<std::uint64_t>(p - base) + 1;
        }
        ++p;
    }
    if (start < data.size() && records.size() < max_records) {
        records.push_back({start, data.size() - start});
    }
    // Blank records are dropped, not just trailing phantoms. A leading empty
    // line would otherwise become `records[0]` and be mistaken for the header
    // row — the file would be reported as headerless with columns named
    // context-free, and a spacer row would skew the delimiter's field count.
    // The indexer skips the same records, so detection and data agree.
    records.erase(std::remove_if(records.begin(), records.end(),
                                 [&data](const RecordView& r) {
                                     const std::string_view raw = data.substr(
                                         static_cast<std::size_t>(r.offset), static_cast<std::size_t>(r.length));
                                     return std::all_of(raw.begin(), raw.end(), [](char c) {
                                         return c == '\r' || c == '\n';
                                     });
                                 }),
                  records.end());
    return records;
}

std::string_view record_text(std::string_view data, const RecordView& r) {
    std::string_view s = data.substr(static_cast<std::size_t>(r.offset), static_cast<std::size_t>(r.length));
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return s;
}

bool looks_numeric(std::string_view field) {
    field = trim(field);
    if (field.empty()) return false;
    double d = 0.0;
    std::int64_t i = 0;
    if (parse_int(field, i)) return true;
    if (parse_double(field, d)) {
        // Pure digits would already have matched parse_int; this catches 1.5, 1e3.
        return !field.empty() && (field[0] == '-' || field[0] == '+' || (field[0] >= '0' && field[0] <= '9') ||
                                  field[0] == '.');
    }
    return false;
}

bool looks_typed(std::string_view field) {
    if (looks_numeric(field)) return true;
    bool b = false;
    if (parse_bool(field, b)) return true;
    return parse_timestamp(field).has_value();
}

bool is_json_line(std::string_view line) {
    line = trim(line);
    return line.size() >= 2 && line.front() == '{' && line.back() == '}';
}

std::string_view strip_bom(std::string_view text) {
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    return text;
}

} // namespace

std::string_view to_string(Format format) {
    switch (format) {
        case Format::Delimited: return "delimited";
        case Format::JsonLines: return "jsonl";
    }
    return "unknown";
}

std::string_view to_string(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8: return "utf-8";
        case Encoding::Utf8Bom: return "utf-8 (bom)";
        case Encoding::Utf16LE: return "utf-16le";
        case Encoding::Utf16BE: return "utf-16be";
        case Encoding::Latin1: return "latin-1 / non-utf8";
    }
    return "unknown";
}

std::string Dialect::delimiter_name() const {
    if (format == Format::JsonLines) return "newline";
    if (delimiter == 0) return "single column";
    switch (delimiter) {
        case ',': return "comma";
        case '\t': return "tab";
        case ';': return "semicolon";
        case '|': return "pipe";
        default: return std::string("'") + delimiter + "'";
    }
}

Encoding detect_encoding(const char* data, std::uint64_t size) {
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        return Encoding::Utf8Bom;
    }
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE) {
        return Encoding::Utf16LE;
    }
    if (size >= 2 && static_cast<unsigned char>(data[0]) == 0xFE && static_cast<unsigned char>(data[1]) == 0xFF) {
        return Encoding::Utf16BE;
    }

    // BOM-less UTF-16: ASCII-heavy text leaves a NUL in every other byte.
    const std::uint64_t probe = std::min<std::uint64_t>(size, 4096);
    if (probe >= 16) {
        std::uint64_t zero_even = 0, zero_odd = 0;
        for (std::uint64_t i = 0; i < probe; ++i) {
            if (data[i] == '\0') {
                if ((i & 1u) == 0) ++zero_even;
                else ++zero_odd;
            }
        }
        const double ratio_even = static_cast<double>(zero_even) / static_cast<double>(probe / 2);
        const double ratio_odd = static_cast<double>(zero_odd) / static_cast<double>(probe / 2);
        if (ratio_odd > 0.3 && ratio_even < 0.05) return Encoding::Utf16LE;
        if (ratio_even > 0.3 && ratio_odd < 0.05) return Encoding::Utf16BE;
    }

    // Validate UTF-8 over a sample; invalid sequences mean a legacy 8-bit code page.
    std::uint64_t i = 0;
    const std::uint64_t limit = std::min<std::uint64_t>(size, 256u * 1024u);
    std::uint64_t bad = 0;
    while (i < limit) {
        const auto byte = static_cast<unsigned char>(data[i]);
        if (byte < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        if ((byte & 0xE0) == 0xC0) extra = 1;
        else if ((byte & 0xF0) == 0xE0) extra = 2;
        else if ((byte & 0xF8) == 0xF0) extra = 3;
        else {
            ++bad;
            ++i;
            continue;
        }
        if (i + static_cast<std::uint64_t>(extra) >= limit) break;
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(data[i + static_cast<std::uint64_t>(k)]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
        }
        if (!ok) ++bad;
        i += static_cast<std::uint64_t>(extra + 1);
    }
    if (bad > 0) return Encoding::Latin1;
    return Encoding::Utf8;
}

std::string utf16_to_utf8(std::string_view bytes, bool little_endian, std::uint64_t& source_bom_bytes) {
    source_bom_bytes = 0;
    std::size_t i = 0;
    if (bytes.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(bytes[0]);
        const auto b1 = static_cast<unsigned char>(bytes[1]);
        if ((b0 == 0xFF && b1 == 0xFE) || (b0 == 0xFE && b1 == 0xFF)) i = 2;
    }
    source_bom_bytes = i;

    std::string out;
    out.reserve(bytes.size());
    const auto read_unit = [&](std::size_t at) -> std::uint32_t {
        const auto lo = static_cast<unsigned char>(bytes[at]);
        const auto hi = static_cast<unsigned char>(bytes[at + 1]);
        return little_endian ? (static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 8))
                             : (static_cast<std::uint32_t>(hi) | (static_cast<std::uint32_t>(lo) << 8));
    };

    while (i + 1 < bytes.size()) {
        std::uint32_t unit = read_unit(i);
        i += 2;
        std::uint32_t cp = unit;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < bytes.size()) {
            const std::uint32_t low = read_unit(i);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
                i += 2;
            } else {
                cp = 0xFFFD;
            }
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            cp = 0xFFFD;
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// ── Field splitting ─────────────────────────────────────────────────────────
void split_fields(std::string_view row, char delimiter, char quote, std::vector<FieldSpan>& out) {
    out.clear();
    const std::size_t n = row.size();
    if (n == 0) {
        out.push_back({0, 0, false});
        return;
    }
    if (delimiter == 0) {
        // Single-column file: the whole record is one field. If that field is
        // quoted, the span must *exclude* the surrounding quotes — the decoder
        // expects that and only unescapes the doubled quotes inside.
        if (quote != 0 && n >= 2 && row.front() == quote && row.back() == quote) {
            out.push_back({1, static_cast<std::uint32_t>(n - 2), true});
        } else {
            out.push_back({0, static_cast<std::uint32_t>(n), false});
        }
        return;
    }

    // Fast path: no quote characters anywhere in the record.
    const bool quoted_row = quote != 0 && std::memchr(row.data(), quote, n) != nullptr;
    if (!quoted_row) {
        std::size_t start = 0;
        const char* base = row.data();
        while (true) {
            const void* hit = std::memchr(base + start, delimiter, n - start);
            if (hit == nullptr) {
                out.push_back({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(n - start), false});
                break;
            }
            const std::size_t pos = static_cast<std::size_t>(static_cast<const char*>(hit) - base);
            out.push_back({static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(pos - start), false});
            start = pos + 1;
        }
        return;
    }

    std::size_t i = 0;
    while (true) {
        if (i < n && row[i] == quote) {
            const std::size_t field_start = i + 1;
            std::size_t j = field_start;
            while (j < n) {
                if (row[j] == quote) {
                    if (j + 1 < n && row[j + 1] == quote) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            const std::size_t field_end = std::min(j, n);
            out.push_back({static_cast<std::uint32_t>(field_start),
                           static_cast<std::uint32_t>(field_end - field_start), true});
            // Skip the closing quote and any stray characters up to the delimiter.
            i = (j < n) ? j + 1 : n;
            while (i < n && row[i] != delimiter) ++i;
        } else {
            std::size_t j = i;
            while (j < n && row[j] != delimiter) ++j;
            out.push_back({static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j - i), false});
            i = j;
        }
        if (i >= n) break;
        ++i; // consume delimiter
        if (i == n) {
            out.push_back({static_cast<std::uint32_t>(n), 0, false});
            break;
        }
    }
}

bool field_at(std::string_view row, char delimiter, char quote, std::uint32_t index, FieldSpan& out) {
    const std::size_t n = row.size();
    if (index == 0 && n == 0) {
        out = {0, 0, false};
        return true;
    }
    if (delimiter == 0) {
        if (index != 0) return false;
        if (quote != 0 && n >= 2 && row.front() == quote && row.back() == quote) {
            out = {1, static_cast<std::uint32_t>(n - 2), true};
        } else {
            out = {0, static_cast<std::uint32_t>(n), false};
        }
        return true;
    }
    const bool quoted_row = quote != 0 && std::memchr(row.data(), quote, n) != nullptr;
    if (!quoted_row) {
        std::size_t start = 0;
        std::uint32_t field = 0;
        const char* base = row.data();
        while (true) {
            const void* hit = std::memchr(base + start, delimiter, n - start);
            const std::size_t end = hit ? static_cast<std::size_t>(static_cast<const char*>(hit) - base) : n;
            if (field == index) {
                out = {static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end - start), false};
                return true;
            }
            if (!hit) return false;
            start = end + 1;
            ++field;
            if (start > n) return false;
        }
    }

    std::size_t i = 0;
    std::uint32_t field = 0;
    while (true) {
        FieldSpan span{};
        if (i < n && row[i] == quote) {
            const std::size_t field_start = i + 1;
            std::size_t j = field_start;
            while (j < n) {
                if (row[j] == quote) {
                    if (j + 1 < n && row[j + 1] == quote) {
                        j += 2;
                        continue;
                    }
                    break;
                }
                ++j;
            }
            const std::size_t field_end = std::min(j, n);
            span = {static_cast<std::uint32_t>(field_start), static_cast<std::uint32_t>(field_end - field_start), true};
            i = (j < n) ? j + 1 : n;
            while (i < n && row[i] != delimiter) ++i;
        } else {
            std::size_t j = i;
            while (j < n && row[j] != delimiter) ++j;
            span = {static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j - i), false};
            i = j;
        }
        if (field == index) {
            out = span;
            return true;
        }
        if (i >= n) return false;
        ++i;
        ++field;
        if (i > n) return false;
    }
}

std::uint32_t count_fields(std::string_view row, char delimiter, char quote) noexcept {
    const std::size_t n = row.size();
    if (n == 0) return 1;
    if (delimiter == 0) return 1; // single-column file
    std::uint32_t count = 1;
    const bool quoted_row = quote != 0 && std::memchr(row.data(), quote, n) != nullptr;
    if (!quoted_row) {
        const char* base = row.data();
        const char* p = base;
        while (true) {
            const void* hit = std::memchr(p, delimiter, static_cast<std::size_t>(base + n - p));
            if (hit == nullptr) break;
            ++count;
            p = static_cast<const char*>(hit) + 1;
        }
        return count;
    }
    bool in_quotes = false;
    std::size_t i = 0;
    while (i < n) {
        const char c = row[i];
        if (c == quote) {
            if (in_quotes && i + 1 < n && row[i + 1] == quote) {
                i += 2;
                continue;
            }
            in_quotes = !in_quotes;
        } else if (c == delimiter && !in_quotes) {
            ++count;
        }
        ++i;
    }
    return count;
}

std::string decode_field(std::string_view field, bool quoted, char quote) {
    if (!quoted) return std::string(field);
    std::string out;
    out.reserve(field.size());
    for (std::size_t i = 0; i < field.size(); ++i) {
        if (field[i] == quote && i + 1 < field.size() && field[i + 1] == quote) {
            out.push_back(quote);
            ++i;
        } else {
            out.push_back(field[i]);
        }
    }
    return out;
}

void append_csv_field(std::string& out, std::string_view value, char delimiter, char quote) {
    const bool needs_quotes = value.find(delimiter) != std::string_view::npos ||
                              value.find(quote) != std::string_view::npos ||
                              value.find('\n') != std::string_view::npos ||
                              value.find('\r') != std::string_view::npos;
    if (!needs_quotes) {
        out.append(value);
        return;
    }
    out.push_back(quote);
    for (const char c : value) {
        out.push_back(c);
        if (c == quote) out.push_back(quote);
    }
    out.push_back(quote);
}

void append_json_string(std::string& out, std::string_view value) {
    static constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const char ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    out.append("\\u00");
                    out.push_back(kHex[(c >> 4) & 0x0F]);
                    out.push_back(kHex[c & 0x0F]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

// ── Sniffing ────────────────────────────────────────────────────────────────
SniffResult sniff(const char* data, std::uint64_t size, const SniffOptions& options) {
    SniffResult result;
    Dialect& d = result.dialect;

    const std::uint64_t limit = std::min<std::uint64_t>(size, kSniffBytes);
    std::string_view raw(data, static_cast<std::size_t>(limit));
    std::string_view text = strip_bom(raw);
    if (text.data() != raw.data()) {
        d.bom_bytes = 3;
        d.notes.emplace_back("UTF-8 byte-order mark removed");
    }
    if (text.data() != raw.data()) d.encoding = Encoding::Utf8Bom;

    d.quotes_present = text.find('"') != std::string_view::npos;
    d.quote = options.allow_quotes ? '"' : '\0';

    const std::size_t crlf_count = [&] {
        std::size_t count = 0;
        for (std::size_t i = 0; i + 1 < text.size() && i < 65536; ++i) {
            if (text[i] == '\r' && text[i + 1] == '\n') ++count;
        }
        return count;
    }();
    d.crlf = crlf_count > 0;

    // JSON Lines / NDJSON detection: extension or shape.
    bool json_by_extension = options.extension == ".jsonl" || options.extension == ".ndjson" || options.extension == ".json";
    if (options.extension == ".json") {
        std::string_view t = trim(text.substr(0, std::min<std::size_t>(text.size(), 4096)));
        if (!t.empty() && t.front() == '[') {
            result.json_array = true;
        }
    }
    if (!result.json_array) {
        const auto records = sample_records(text, '"', 8);
        std::size_t json_like = 0;
        for (const auto& r : records) {
            if (is_json_line(record_text(text, r))) ++json_like;
        }
        const bool shape_says_json = !records.empty() && json_like >= std::min<std::size_t>(records.size(), 4);
        if (json_like > 0 && (json_by_extension || shape_says_json)) {
            d.format = Format::JsonLines;
            d.quote = '"';
            d.has_header = true;
            d.notes.emplace_back(json_by_extension ? "JSON Lines detected from file extension"
                                                   : "JSON Lines detected from record shape");
            return result;
        }
    }

    d.format = Format::Delimited;
    d.encoding = d.encoding == Encoding::Utf8Bom ? Encoding::Utf8Bom : Encoding::Utf8;

    const auto records = sample_records(text, d.quote, 200);

    // ── Delimiter: pick the candidate that produces the most consistent shape.
    char best = ',';
    if (options.force_delimiter) {
        best = *options.force_delimiter;
        d.delimiter = best;
        d.notes.emplace_back("delimiter forced by the command line");
    } else {
        // The single-column hypothesis is the baseline: one field per record is
        // consistent by definition, so no candidate delimiter wins unless it
        // explains a clear majority of the sampled records. Without this, one
        // stray comma inside a one-column export (an ID list, a log dump) turns
        // the file into two columns, the second of them empty for every row.
        // A delimiter must reach this share of the sample to be believed.
        constexpr double kMinConsistency = 0.5;
        double best_score = -1.0;
        double best_consistency = 0.0;
        std::string best_explain;
        for (const char candidate : kCandidateDelimiters) {
            if (text.find(candidate) == std::string_view::npos) continue;
            std::vector<std::uint32_t> counts;
            counts.reserve(records.size());
            for (const auto& r : records) {
                counts.push_back(count_fields(record_text(text, r), candidate, d.quote));
            }
            if (counts.empty()) continue;
            // Modal field count and how many records agree with it.
            std::vector<std::uint32_t> sorted(counts);
            std::sort(sorted.begin(), sorted.end());
            std::uint32_t modal = 0;
            std::size_t modal_runs = 0;
            for (std::size_t i = 0; i < sorted.size();) {
                std::size_t j = i;
                while (j < sorted.size() && sorted[j] == sorted[i]) ++j;
                if (j - i > modal_runs) {
                    modal_runs = j - i;
                    modal = sorted[i];
                }
                i = j;
            }
            if (modal <= 1) continue;
            const double consistency = static_cast<double>(modal_runs) / static_cast<double>(counts.size());
            // Favour richer records, but consistency dominates.
            const double score = consistency * 100.0 + static_cast<double>(modal);
            if (score > best_score) {
                best_score = score;
                best_consistency = consistency;
                best = candidate;
                const std::string name = (candidate == '\t') ? "tab" : std::string(1, candidate);
                best_explain = name + " (" + format_count(static_cast<std::uint64_t>(modal)) + " fields, " +
                               format_percent(consistency, 0) + " of sampled records agree)";
            }
        }
        if (!best_explain.empty() && best_consistency >= kMinConsistency) {
            d.delimiter = best;
            d.notes.push_back("delimiter: " + best_explain);
        } else {
            // Zero means "no delimiter": the record is one field. This is a
            // real format (single-column exports), not a failure to detect,
            // and saying so is more useful than guessing a delimiter that
            // would invent empty columns.
            d.delimiter = 0;
            if (best_explain.empty()) {
                d.notes.emplace_back("delimiter: none — single-column file (no candidate delimiter appears)");
            } else {
                d.notes.emplace_back("delimiter: none — no delimiter explains the records consistently "
                                     "(best was ' " +
                                     std::string(1, best) + "' at " + format_percent(best_consistency, 0) +
                                     "); pass --delimiter to override");
            }
        }
    }

    // ── Header detection: a header is text-only and sits above typed columns.
    bool header = false;
    if (records.size() >= 2) {
        std::vector<FieldSpan> first_fields, second_fields;
        split_fields(record_text(text, records[0]), d.delimiter, d.quote, first_fields);
        split_fields(record_text(text, records[1]), d.delimiter, d.quote, second_fields);

        std::size_t typed_first = 0, typed_second = 0;
        for (const auto& f : first_fields) {
            const auto span = std::string_view(record_text(text, records[0])).substr(f.offset, f.length);
            if (looks_typed(span)) ++typed_first;
        }
        for (const auto& f : second_fields) {
            const auto span = std::string_view(record_text(text, records[1])).substr(f.offset, f.length);
            if (looks_typed(span)) ++typed_second;
        }
        // Names must be non-empty for a convincing header row. Duplicate names
        // are *not* disqualifying: they are common in exports of joined tables,
        // and Dataset disambiguates them (`id`, `id_2`) so queries stay
        // addressable. A first row whose fields are all identical is a different
        // story — `x,x,x` is data, not three column names.
        bool names_ok = !first_fields.empty();
        bool all_identical = first_fields.size() > 1;
        std::string_view first_name;
        for (std::size_t i = 0; i < first_fields.size() && names_ok; ++i) {
            const auto name =
                trim(std::string_view(record_text(text, records[0])).substr(first_fields[i].offset, first_fields[i].length));
            if (name.empty()) {
                names_ok = false;
                break;
            }
            if (i == 0) {
                first_name = name;
            } else if (!iequals(first_name, name)) {
                all_identical = false;
            }
        }
        if (names_ok && all_identical) names_ok = false;
        header = names_ok && typed_first == 0 && typed_second > 0;
        if (!header && names_ok && typed_first == 0 && typed_second == 0) {
            // All-text file: a header is still likely if names look like identifiers.
            bool identifier_like = !first_fields.empty();
            for (const auto& f : first_fields) {
                const auto name = trim(std::string_view(record_text(text, records[0])).substr(f.offset, f.length));
                if (name.empty() || name.size() > 64) {
                    identifier_like = false;
                    break;
                }
                for (const char c : name) {
                    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                    c == '_' || c == ' ' || c == '-' || c == '.' || c == '/';
                    if (!ok) {
                        identifier_like = false;
                        break;
                    }
                }
                if (!identifier_like) break;
            }
            if (identifier_like && records.size() >= 8) header = true;
        }
        if (header) d.notes.emplace_back("first record treated as a header (names are text, data below is typed)");
        else d.notes.emplace_back("no header row detected (first record contains values)");
    } else if (records.size() == 1) {
        header = true;
        d.notes.emplace_back("single-record file: treated as a header");
    }

    if (options.force_header) {
        header = *options.force_header;
        d.notes.emplace_back(header ? "--header forced" : "--no-header forced");
    }
    d.has_header = header;

    if (!records.empty()) {
        result.first_record_fields = count_fields(record_text(text, records[0]), d.delimiter, d.quote);
    }
    return result;
}

} // namespace tess
