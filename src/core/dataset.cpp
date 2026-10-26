// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · dataset.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/dataset.hpp"

#include "common/util.hpp"
#include "core/indexer.hpp"
#include "core/sidecar.hpp"
#include "core/simd.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace tess {
namespace {

constexpr std::uint32_t kMaxColumns = 2048;

std::string extension_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return to_lower(path.substr(dot));
}

std::uint64_t first_record_end(const char* data, std::uint64_t size, std::uint64_t begin, char quote) {
    if (begin >= size) return begin;
    if (quote == 0) {
        const char* end = data + size;
        const char* hit = swar::find_byte(data + begin, end, '\n');
        return static_cast<std::uint64_t>(hit - data);
    }
    bool in_quotes = false;
    for (std::uint64_t i = begin; i < size; ++i) {
        const char c = data[i];
        if (c == quote) {
            in_quotes = !in_quotes;
        } else if (c == '\n' && !in_quotes) {
            return i;
        }
    }
    return size;
}

// ── Minimal JSON reader for JSON Lines materialisation ──────────────────────
class JsonScanner {
public:
    explicit JsonScanner(std::string_view text) : text_(text) {}

    bool parse_object(std::vector<std::pair<std::string, std::string>>& out) {
        skip_ws();
        if (!consume('{')) return false;
        skip_ws();
        if (consume('}')) return true;
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (!consume(':')) return false;
            skip_ws();
            std::string value;
            if (!capture_value(value)) return false;
            out.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

    void skip_ws() {
        while (i_ < text_.size()) {
            const char c = text_[i_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++i_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool at_end() {
        skip_ws();
        return i_ >= text_.size();
    }

private:
    bool consume(char c) {
        if (i_ < text_.size() && text_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }

    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (i_ < text_.size()) {
            const char c = text_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= text_.size()) return false;
            const char esc = text_[i_++];
            switch (esc) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'u': {
                    std::uint32_t cp = 0;
                    if (!read_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < text_.size() && text_[i_] == '\\' &&
                        text_[i_ + 1] == 'u') {
                        i_ += 2;
                        std::uint32_t low = 0;
                        if (!read_hex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: out.push_back(esc); break;
            }
        }
        return false;
    }

    bool read_hex4(std::uint32_t& out) {
        if (i_ + 4 > text_.size()) return false;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
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

    bool capture_value(std::string& out) {
        skip_ws();
        if (i_ >= text_.size()) return false;
        const char c = text_[i_];
        if (c == '"') return parse_string(out);
        if (c == '{' || c == '[') {
            const std::size_t start = i_;
            int depth = 0;
            bool in_string = false;
            while (i_ < text_.size()) {
                const char ch = text_[i_];
                if (in_string) {
                    if (ch == '\\') {
                        i_ += 2;
                        continue;
                    }
                    if (ch == '"') in_string = false;
                } else if (ch == '"') {
                    in_string = true;
                } else if (ch == '{' || ch == '[') {
                    ++depth;
                } else if (ch == '}' || ch == ']') {
                    --depth;
                    if (depth == 0) {
                        ++i_;
                        out.assign(text_.substr(start, i_ - start));
                        return true;
                    }
                }
                ++i_;
            }
            return false;
        }
        const std::size_t start = i_;
        while (i_ < text_.size()) {
            const char ch = text_[i_];
            if (ch == ',' || ch == '}' || ch == ']' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
            ++i_;
        }
        std::string_view token = text_.substr(start, i_ - start);
        if (token.empty()) return false;
        if (iequals(token, "null")) {
            out.clear();
        } else {
            out.assign(token);
        }
        return true;
    }

    std::string_view text_;
    std::size_t i_ = 0;
};

struct TypeCounter {
    std::uint64_t total = 0;
    std::uint64_t nulls = 0;
    std::uint64_t ints = 0;
    std::uint64_t floats = 0;
    std::uint64_t bools = 0;
    std::uint64_t times = 0;
    std::uint64_t texts = 0;
    std::uint32_t width = 0;
};

void classify_value(std::string_view raw, TypeCounter& counter) {
    const std::string_view value = trim(raw);
    ++counter.total;
    if (value.empty()) {
        ++counter.nulls;
        return;
    }
    const std::uint32_t width = static_cast<std::uint32_t>(std::min<int>(text_width(value), 40));
    counter.width = std::max(counter.width, width);

    std::int64_t i = 0;
    if (parse_int(value, i)) {
        ++counter.ints;
        return;
    }
    double d = 0.0;
    if (parse_double(value, d)) {
        ++counter.floats;
        return;
    }
    bool b = false;
    if (parse_bool(value, b)) {
        ++counter.bools;
        return;
    }
    if (parse_timestamp(value).has_value()) {
        ++counter.times;
        return;
    }
    ++counter.texts;
}

ColumnType resolve_type(const TypeCounter& c) {
    const std::uint64_t non_null = c.total - c.nulls;
    if (non_null == 0) return ColumnType::Unknown;
    if (c.times == non_null) return ColumnType::Timestamp;
    if (c.ints == non_null) return ColumnType::Integer;
    if (c.ints + c.floats == non_null) return ColumnType::Float;
    if (c.bools == non_null) return ColumnType::Boolean;
    return ColumnType::Text;
}

double confidence_of(const TypeCounter& c, ColumnType type) {
    const std::uint64_t non_null = c.total - c.nulls;
    if (non_null == 0) return 0.0;
    std::uint64_t matching = 0;
    switch (type) {
        case ColumnType::Integer: matching = c.ints; break;
        case ColumnType::Float: matching = c.ints + c.floats; break;
        case ColumnType::Boolean: matching = c.bools; break;
        case ColumnType::Timestamp: matching = c.times; break;
        case ColumnType::Text: matching = c.texts; break;
        case ColumnType::Unknown: return 0.0;
    }
    return static_cast<double>(matching) / static_cast<double>(non_null);
}

/// Records are sampled from the head plus a stride across the file so a sorted
/// export does not convince the reader that column 7 is all nulls.
std::vector<std::uint64_t> sample_rows(std::uint64_t row_count, std::size_t limit) {
    std::vector<std::uint64_t> rows;
    if (row_count == 0 || limit == 0) return rows;
    if (row_count <= limit) {
        rows.resize(static_cast<std::size_t>(row_count));
        for (std::uint64_t i = 0; i < row_count; ++i) rows[static_cast<std::size_t>(i)] = i;
        return rows;
    }
    const std::size_t head = std::min<std::size_t>(limit / 2, 512);
    rows.reserve(limit);
    for (std::size_t i = 0; i < head; ++i) rows.push_back(i);
    const std::size_t remaining = limit - head;
    const std::uint64_t stride = std::max<std::uint64_t>(1, row_count / remaining);
    for (std::size_t i = 0; i < remaining; ++i) {
        const std::uint64_t row = std::min<std::uint64_t>(row_count - 1, (static_cast<std::uint64_t>(i) + 1) * stride);
        rows.push_back(row);
    }
    return rows;
}

} // namespace

// ── Column type helpers ─────────────────────────────────────────────────────
std::string_view to_string(ColumnType type) {
    switch (type) {
        case ColumnType::Integer: return "integer";
        case ColumnType::Float: return "float";
        case ColumnType::Boolean: return "boolean";
        case ColumnType::Timestamp: return "timestamp";
        case ColumnType::Text: return "text";
        case ColumnType::Unknown: return "empty";
    }
    return "text";
}

bool is_numeric(ColumnType type) noexcept {
    return type == ColumnType::Integer || type == ColumnType::Float;
}

char type_glyph(ColumnType type) noexcept {
    switch (type) {
        case ColumnType::Integer: return '#';
        case ColumnType::Float: return '~';
        case ColumnType::Boolean: return '?';
        case ColumnType::Timestamp: return '@';
        case ColumnType::Text: return 'a';
        case ColumnType::Unknown: return '-';
    }
    return 'a';
}

// ── JSON Lines ──────────────────────────────────────────────────────────────
std::string jsonl_to_csv(std::string_view jsonl, std::vector<std::string>& columns_out,
                         std::uint64_t* skipped) {
    columns_out.clear();
    std::uint64_t bad = 0;

    std::vector<std::pair<std::string, std::string>> pairs;
    std::unordered_map<std::string, std::uint32_t> index;

    const auto add_column = [&](const std::string& name) {
        if (index.find(name) != index.end()) return;
        index.emplace(name, static_cast<std::uint32_t>(columns_out.size()));
        columns_out.push_back(name);
    };

    // Pass 1 — key union from a bounded prefix keeps the common case O(1) memory.
    {
        std::size_t start = 0;
        std::size_t seen = 0;
        while (start < jsonl.size() && seen < 2048) {
            const std::size_t nl = jsonl.find('\n', start);
            const std::size_t end = (nl == std::string_view::npos) ? jsonl.size() : nl;
            std::string_view line = jsonl.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            start = end + 1;
            if (trim(line).empty()) continue;
            ++seen;
            pairs.clear();
            JsonScanner scanner(line);
            if (!scanner.parse_object(pairs)) continue;
            for (const auto& kv : pairs) add_column(kv.first);
        }
    }
    if (columns_out.empty()) {
        if (skipped != nullptr) *skipped = bad;
        return std::string();
    }

    std::string out;
    out.reserve(jsonl.size() + jsonl.size() / 4);
    for (std::size_t i = 0; i < columns_out.size(); ++i) {
        if (i) out.push_back(',');
        append_csv_field(out, columns_out[i], ',', '"');
    }
    out.push_back('\n');

    std::vector<std::string> values(columns_out.size());
    std::vector<std::uint8_t> present(columns_out.size(), 0);
    std::size_t start = 0;
    while (start < jsonl.size()) {
        const std::size_t nl = jsonl.find('\n', start);
        const std::size_t end = (nl == std::string_view::npos) ? jsonl.size() : nl;
        std::string_view line = jsonl.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        start = end + 1;
        if (trim(line).empty()) continue;

        pairs.clear();
        JsonScanner scanner(line);
        if (!scanner.parse_object(pairs)) {
            ++bad;
            continue;
        }
        std::size_t highest = 0;
        for (const auto& kv : pairs) {
            auto it = index.find(kv.first);
            if (it == index.end()) {
                const std::uint32_t new_index = static_cast<std::uint32_t>(columns_out.size());
                index.emplace(kv.first, new_index);
                columns_out.push_back(kv.first);
                values.emplace_back();
                present.emplace_back();
                it = index.find(kv.first);
            }
            const std::uint32_t col = it->second;
            values[col] = kv.second;
            present[col] = 1;
            highest = std::max<std::size_t>(highest, col);
        }
        for (std::size_t i = 0; i <= highest && i < values.size(); ++i) {
            if (i) out.push_back(',');
            if (present[i]) append_csv_field(out, values[i], ',', '"');
        }
        out.push_back('\n');
        std::fill(present.begin(), present.end(), static_cast<std::uint8_t>(0));
        for (std::size_t i = 0; i <= highest && i < values.size(); ++i) values[i].clear();
    }

    if (skipped != nullptr) *skipped = bad;
    return out;
}

// ── Dataset ─────────────────────────────────────────────────────────────────
std::shared_ptr<Dataset> Dataset::open(std::string path, const OpenOptions& options) {
    const double started = now_seconds();
    auto ds = std::shared_ptr<Dataset>(new Dataset());
    ds->source_ = Source::open_file(path);
    ds->info_.path = path;
    ds->info_.name = ds->source_->name();

    const std::string ext = extension_of(path);
    if (ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".zip" || ext == ".7z" || ext == ".zst") {
        fail("'" + ds->info_.name + "' is compressed (" + ext +
             "). Decompress it first, for example: gunzip -k " + ds->info_.name);
    }

    const Encoding encoding = detect_encoding(ds->source_->data(), ds->source_->size());
    if (encoding == Encoding::Utf16LE || encoding == Encoding::Utf16BE) {
        std::uint64_t bom = 0;
        std::string utf8 = utf16_to_utf8(ds->source_->all(), encoding == Encoding::Utf16LE, bom);
        const std::string name = ds->info_.name;
        ds->source_ = Source::from_buffer(std::move(utf8), name, path);
        ds->materialized_ = true;
        ds->info_.encoding = encoding;
        ds->info_.notes.push_back("transcoded from " + std::string(to_string(encoding)) + " to UTF-8");
    } else if (encoding == Encoding::Latin1) {
        ds->info_.encoding = Encoding::Latin1;
        ds->info_.notes.emplace_back("file is not valid UTF-8; bytes are shown as-is (legacy code page)");
    } else {
        ds->info_.encoding = encoding;
    }

    ds->initialise(options, started);
    return ds;
}

std::shared_ptr<Dataset> Dataset::parse(std::string text, std::string name, const OpenOptions& options) {
    const double started = now_seconds();
    auto ds = std::shared_ptr<Dataset>(new Dataset());
    ds->source_ = Source::from_buffer(std::move(text), std::move(name));
    ds->info_.name = ds->source_->name();
    ds->info_.encoding = Encoding::Utf8;
    ds->initialise(options, started);
    return ds;
}

void Dataset::initialise(const OpenOptions& options, double open_start) {
    const char* data = source_->data();
    const std::uint64_t size = source_->size();

    if (size == 0) {
        info_.notes.emplace_back("file is empty");
        info_.bytes = 0;
        info_.rows = 0;
        info_.columns = 0;
        info_.open_seconds = now_seconds() - open_start;
        return;
    }

    // Reject binary files early with an actionable message instead of a grid
    // full of replacement characters.
    if (info_.encoding == Encoding::Utf8 || info_.encoding == Encoding::Utf8Bom) {
        std::uint64_t nuls = 0;
        const std::uint64_t probe = std::min<std::uint64_t>(size, 8192);
        for (std::uint64_t i = 0; i < probe; ++i) {
            if (data[i] == '\0') ++nuls;
        }
        if (nuls > 8) {
            fail("'" + info_.name + "' looks like a binary file, not delimited text");
        }
    }

    SniffOptions sniff_options;
    sniff_options.force_delimiter = options.delimiter;
    sniff_options.force_header = options.header;
    sniff_options.allow_quotes = options.quotes;
    sniff_options.extension = extension_of(info_.path.empty() ? info_.name : info_.path);

    SniffResult sniffed = sniff(data, size, sniff_options);
    dialect_ = sniffed.dialect;
    if (sniffed.json_array) {
        fail("'" + info_.name + "' is a JSON array, not JSON Lines. Convert it first, e.g. "
             "`jq -c '.[]' file.json > file.jsonl`");
    }
    info_.notes = dialect_.notes;

    if (dialect_.format == Format::JsonLines) {
        std::vector<std::string> json_columns;
        std::uint64_t skipped = 0;
        std::string csv = jsonl_to_csv(source_->all(), json_columns, &skipped);
        if (csv.empty()) {
            fail("'" + info_.name + "' contains no JSON objects on separate lines");
        }
        info_.format = Format::JsonLines;
        const std::string name = info_.name;
        const std::string path = info_.path;
        source_ = Source::from_buffer(std::move(csv), name, path);
        materialized_ = true;

        dialect_.format = Format::Delimited;
        dialect_.delimiter = ',';
        dialect_.quote = '"';
        dialect_.has_header = true;
        dialect_.bom_bytes = 0;
        data = source_->data();
        info_.notes.emplace_back("JSON Lines materialised into " + format_count(json_columns.size()) +
                                 " columns (one pass, kept in memory)");
        if (skipped > 0) {
            info_.notes.push_back(format_count(skipped) + " malformed JSON records were skipped");
        }
    }

    const char* const bytes = data;
    const std::uint64_t byte_count = source_->size();
    const std::uint64_t begin = dialect_.bom_bytes;

    // Header signature: stable across runs, sensitive to column layout.
    const std::uint64_t record_end = first_record_end(bytes, byte_count, begin, dialect_.quote);
    std::uint64_t signature = fnv1a(std::string_view(bytes + begin,
                                                     static_cast<std::size_t>(std::min<std::uint64_t>(
                                                         record_end - begin, 4096))));
    signature = fnv1a(std::string_view(&dialect_.delimiter, 1), signature);
    signature = fnv1a(dialect_.has_header ? "h1" : "h0", signature);
    header_signature_ = signature;

    info_.delimiter = dialect_.delimiter;
    info_.quote = dialect_.quote;
    info_.has_header = dialect_.has_header;
    info_.crlf = dialect_.crlf;
    info_.bytes = byte_count;
    info_.format = info_.format == Format::JsonLines ? Format::JsonLines : Format::Delimited;

    // A single 4 GiB+ record would be pathological; still, the index handles it.
    bool used_cache = false;
    if (options.use_sidecar && !materialized_ && source_->is_file()) {
        IndexCacheInfo cache_info;
        auto cached = load_index_cache(info_.path, byte_count, source_->mtime_unix(), signature, &cache_info);
        if (cached.has_value()) {
            starts_ = std::move(*cached);
            used_cache = true;
            info_.index_from_sidecar = true;
            info_.index_bytes = cache_info.bytes;
            info_.notes.emplace_back("row index restored from " + std::string(".tessera.idx") + " (" +
                                     format_bytes(cache_info.bytes) + ")");
        }
    }

    IndexStats index_stats;
    if (!used_cache) {
        build_row_index(bytes, byte_count, begin, dialect_.quote, options.threads, options.cancel,
                        options.progress, starts_, index_stats);
        info_.index_seconds = index_stats.seconds;
        info_.index_bytes = starts_.size() * sizeof(std::uint64_t);
        if (index_stats.repaired_quotes) {
            info_.notes.emplace_back("unbalanced quotes detected; quoting was repaired during indexing");
        }
    }

    read_header(options);
    infer_types(options);

    if (options.write_sidecar && !materialized_ && source_->is_file() && !used_cache) {
        if (tess::save_index_cache(info_.path, byte_count, source_->mtime_unix(), signature, starts_,
                             dialect_.delimiter, dialect_.quote, dialect_.has_header)) {
            info_.index_from_sidecar = true;
            info_.notes.emplace_back("row index cached to " + index_cache_path(info_.name));
        }
    }

    info_.open_seconds = now_seconds() - open_start;
}

void Dataset::read_header(const OpenOptions& options) {
    (void)options;
    if (starts_.empty()) {
        data_rows_ = 0;
        first_data_row_ = 0;
        columns_.clear();
        return;
    }

    const std::string_view first = row_text(0);
    if (dialect_.has_header && trim(first).empty()) {
        dialect_.has_header = false;
        info_.has_header = false;
        info_.notes.emplace_back("first record was empty; header detection disabled");
    }

    // Row accessors clamp against data_rows_, so the extent must be known
    // before the column-count sample below reads any record.
    first_data_row_ = dialect_.has_header ? 1u : 0u;
    data_rows_ = starts_.size() - first_data_row_;

    std::vector<FieldSpan> spans;
    std::uint32_t header_fields = 0;
    if (dialect_.has_header) {
        split_fields(first, dialect_.delimiter, dialect_.quote, spans);
        header_fields = static_cast<std::uint32_t>(spans.size());
    }

    // Column count: the widest record among a spread of samples, so ragged
    // exports do not hide trailing columns.
    const std::uint64_t total_records = starts_.size();
    const std::uint64_t data_count = total_records - (dialect_.has_header ? 1u : 0u);
    std::uint32_t max_fields = header_fields;
    const auto rows = sample_rows(data_count, 256);
    for (const std::uint64_t row : rows) {
        const std::string_view text = row_text(row);
        const std::uint32_t count = count_fields(text, dialect_.delimiter, dialect_.quote);
        max_fields = std::max(max_fields, count);
    }
    max_fields = std::min(max_fields, kMaxColumns);
    if (max_fields == 0) max_fields = 1;

    columns_.clear();
    columns_.reserve(max_fields);
    std::vector<std::string> used;
    used.reserve(max_fields);
    std::uint32_t renamed_duplicates = 0;
    for (std::uint32_t i = 0; i < max_fields; ++i) {
        ColumnMeta meta;
        if (i < spans.size()) {
            meta.name = sanitize(first.substr(spans[i].offset, spans[i].length));
            meta.name = std::string(trim(meta.name));
        }
        if (meta.name.empty()) {
            meta.name = "column_" + std::to_string(i + 1);
        }
        // Deduplicate case-insensitively; duplicate names break nothing but
        // make `ORDER BY name` ambiguous, so disambiguate early.
        std::string candidate = meta.name;
        int suffix = 2;
        bool clash = true;
        while (clash) {
            clash = false;
            for (const auto& existing : used) {
                if (iequals(existing, candidate)) {
                    clash = true;
                    break;
                }
            }
            if (clash) {
                candidate = meta.name + "_" + std::to_string(suffix);
                ++suffix;
            }
        }
        if (candidate != meta.name) ++renamed_duplicates;
        meta.name = candidate;
        used.push_back(meta.name);
        meta.sample_width = static_cast<std::uint32_t>(std::min<int>(std::max<int>(text_width(meta.name), 4), 40));
        columns_.push_back(std::move(meta));
    }

    if (renamed_duplicates > 0) {
        info_.notes.emplace_back(format_count(renamed_duplicates) +
                                 " duplicate column name(s) were made unique with a numeric suffix");
    }
}

void Dataset::infer_types(const OpenOptions& options) {
    if (columns_.empty()) return;
    const std::uint64_t row_count = data_rows_;
    std::vector<TypeCounter> counters(columns_.size());
    std::uint64_t nulls = 0;
    std::uint64_t ragged = 0;
    std::uint64_t widest = 0;

    const auto rows = sample_rows(row_count, options.infer_rows);
    std::vector<FieldSpan> spans;
    for (const std::uint64_t row : rows) {
        const std::string_view text = row_text(row);
        widest = std::max<std::uint64_t>(widest, text.size());
        split_fields(text, dialect_.delimiter, dialect_.quote, spans);
        if (spans.size() < columns_.size()) ++ragged;
        for (std::size_t col = 0; col < counters.size(); ++col) {
            if (col < spans.size()) {
                classify_value(text.substr(spans[col].offset, spans[col].length), counters[col]);
            } else {
                ++counters[col].total;
                ++counters[col].nulls;
            }
        }
    }

    for (std::size_t i = 0; i < columns_.size(); ++i) {
        const ColumnType type = resolve_type(counters[i]);
        columns_[i].type = type;
        columns_[i].type_confidence = confidence_of(counters[i], type);
        columns_[i].sample_nulls = static_cast<std::uint32_t>(counters[i].nulls);
        columns_[i].sample_width = std::max<std::uint32_t>(columns_[i].sample_width, counters[i].width);
        nulls += counters[i].nulls;
    }

    info_.null_cells_sampled = nulls;
    info_.ragged_rows = ragged;
    info_.widest_row_bytes = widest;
    info_.rows = row_count;
    info_.columns = static_cast<std::uint32_t>(columns_.size());
}

std::string_view Dataset::column_name(std::uint32_t index) const noexcept {
    if (index >= columns_.size()) return {};
    return columns_[index].name;
}

ColumnType Dataset::column_type(std::uint32_t index) const noexcept {
    if (index >= columns_.size()) return ColumnType::Unknown;
    return columns_[index].type;
}

std::optional<std::uint32_t> Dataset::column_index(std::string_view name) const noexcept {
    const std::string_view needle = trim(name);
    for (std::size_t i = 0; i < columns_.size(); ++i) {
        if (iequals(columns_[i].name, needle)) return static_cast<std::uint32_t>(i);
    }
    // Allow numeric references such as $3 for the third column.
    if (!needle.empty() && (needle[0] == '$' || needle[0] == '#')) {
        std::int64_t v = 0;
        if (parse_int(needle.substr(1), v) && v >= 1 && v <= static_cast<std::int64_t>(columns_.size())) {
            return static_cast<std::uint32_t>(v - 1);
        }
    }
    return std::nullopt;
}

std::uint64_t Dataset::row_offset(std::uint64_t row) const {
    const std::uint64_t index = row + first_data_row_;
    if (index >= starts_.size()) return source_->size();
    return starts_[static_cast<std::size_t>(index)];
}

std::string_view Dataset::row_text(std::uint64_t row) const {
    const std::uint64_t index = row + first_data_row_;
    if (index >= starts_.size()) return {};
    const std::uint64_t start = starts_[static_cast<std::size_t>(index)];
    const std::uint64_t raw_end = (index + 1 < starts_.size()) ? starts_[static_cast<std::size_t>(index + 1)]
                                                               : source_->size();
    std::uint64_t end = raw_end;
    const char* data = source_->data();
    if (end > start && data[end - 1] == '\n') --end;
    if (end > start && data[end - 1] == '\r') --end;
    if (end <= start) return {};
    return std::string_view(data + start, static_cast<std::size_t>(end - start));
}

std::string_view Dataset::cell(std::uint64_t row, std::uint32_t column) const {
    const std::string_view text = row_text(row);
    FieldSpan span;
    if (!field_at(text, dialect_.delimiter, dialect_.quote, column, span)) return {};
    return text.substr(span.offset, span.length);
}

std::string Dataset::decode_cell(std::uint64_t row, std::uint32_t column) const {
    const std::string_view text = row_text(row);
    FieldSpan span;
    if (!field_at(text, dialect_.delimiter, dialect_.quote, column, span)) return {};
    const std::string_view raw = text.substr(span.offset, span.length);
    if (!span.quoted || raw.find(dialect_.quote) == std::string_view::npos) return std::string(raw);
    return decode_field(raw, true, dialect_.quote);
}

void Dataset::row_fields(std::uint64_t row, std::vector<FieldSpan>& scratch) const {
    split_fields(row_text(row), dialect_.delimiter, dialect_.quote, scratch);
}

std::string_view Dataset::field_text(std::string_view row, const FieldSpan& span) const {
    if (span.offset > row.size()) return {};
    return row.substr(span.offset, std::min<std::size_t>(span.length, row.size() - span.offset));
}

std::optional<std::int64_t> Dataset::cell_int(std::uint64_t row, std::uint32_t column) const {
    const std::string_view text = trim(cell(row, column));
    std::int64_t value = 0;
    if (!parse_int(text, value)) return std::nullopt;
    return value;
}

std::optional<double> Dataset::cell_double(std::uint64_t row, std::uint32_t column) const {
    const std::string_view text = trim(cell(row, column));
    double value = 0.0;
    if (!parse_double(text, value)) return std::nullopt;
    return value;
}

std::uint64_t Dataset::save_index_cache() const {
    if (materialized_ || !source_->is_file()) return 0;
    const bool ok = tess::save_index_cache(info_.path, info_.bytes, source_->mtime_unix(), header_signature_,
                                          starts_, dialect_.delimiter, dialect_.quote, dialect_.has_header);
    if (!ok) return 0;
    IndexCacheInfo cache_info;
    if (auto loaded = load_index_cache(info_.path, info_.bytes, source_->mtime_unix(), header_signature_,
                                       &cache_info)) {
        return cache_info.bytes;
    }
    return 0;
}

} // namespace tess
