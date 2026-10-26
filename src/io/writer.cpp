// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · writer.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "io/writer.hpp"

#include "common/error.hpp"
#include "common/util.hpp"
#include "core/dialect.hpp"

#include <cstdio>
#include <cstring>

namespace tess::io {
namespace {

constexpr std::size_t kFlushThreshold = 1u << 20; // 1 MiB

/// Appends a field exactly as it appears in the source, preserving whether it
/// was quoted. This makes CSV -> CSV round-trips byte-identical.
void append_raw_field(std::string& out, std::string_view row, const FieldSpan& span, char delimiter, char quote) {
    const std::string_view value = row.substr(span.offset, span.length);
    if (span.quoted) {
        out.push_back(quote);
        out.append(value);
        out.push_back(quote);
        return;
    }
    // Unquoted fields cannot legally contain the delimiter or a quote, but
    // broken files do exist; escape defensively rather than emit ambiguity.
    const bool needs_quoting = value.find(delimiter) != std::string_view::npos ||
                               value.find(quote) != std::string_view::npos ||
                               value.find('\n') != std::string_view::npos;
    if (needs_quoting) {
        append_csv_field(out, value, delimiter, quote);
    } else {
        out.append(value);
    }
}

} // namespace

OutputFormat parse_output_format(std::string_view name) {
    const std::string lowered = to_lower(trim(name));
    if (lowered == "csv") return OutputFormat::Csv;
    if (lowered == "tsv" || lowered == "tab") return OutputFormat::Tsv;
    if (lowered == "jsonl" || lowered == "ndjson" || lowered == "json") return OutputFormat::JsonLines;
    fail("unknown output format '" + std::string(name) + "' (expected csv, tsv or jsonl)");
}

std::string to_string(OutputFormat format) {
    switch (format) {
        case OutputFormat::Csv: return "csv";
        case OutputFormat::Tsv: return "tsv";
        case OutputFormat::JsonLines: return "jsonl";
    }
    return "csv";
}

ExportStats export_rows(const Dataset& dataset, const std::vector<std::uint64_t>* rows,
                        const std::vector<std::uint32_t>& columns, const std::string& path,
                        OutputFormat format, const query::RunOptions& options) {
    const double started = now_seconds();
    ExportStats stats;

    std::vector<std::uint32_t> targets = columns;
    if (targets.empty()) {
        for (std::uint32_t c = 0; c < dataset.column_count(); ++c) targets.push_back(c);
    }
    for (const std::uint32_t column : targets) {
        if (column >= dataset.column_count()) {
            fail("column index " + std::to_string(column) + " is out of range");
        }
    }

    const bool to_stdout = (path == "-");
    std::FILE* file = to_stdout ? stdout : fopen_utf8(path, "wb");
    if (file == nullptr) fail("cannot create '" + path + "'");
    if (to_stdout) {
        std::setvbuf(file, nullptr, _IOFBF, 1 << 20);
    }

    const char delimiter = format == OutputFormat::Tsv ? '\t' : ',';
    const char quote = '"';
    std::string buffer;
    buffer.reserve(kFlushThreshold + 4096);
    bool failed = false;

    const auto flush = [&]() {
        if (buffer.empty() || failed) return;
        if (std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size()) failed = true;
        stats.bytes += buffer.size();
        buffer.clear();
    };

    if (format != OutputFormat::JsonLines) {
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (i > 0) buffer.push_back(delimiter);
            append_csv_field(buffer, dataset.column_name(targets[i]), delimiter, quote);
        }
        buffer.push_back('\n');
    }

    const std::uint64_t total = rows != nullptr ? rows->size() : dataset.row_count();
    std::vector<FieldSpan> spans;
    for (std::uint64_t i = 0; i < total; ++i) {
        if ((i & 65535u) == 0) {
            if (options.cancel != nullptr) options.cancel->throw_if_canceled();
            if (options.progress && total > 0) {
                options.progress(static_cast<double>(i) / static_cast<double>(total), "exporting");
            }
        }
        const std::uint64_t row = rows != nullptr ? (*rows)[static_cast<std::size_t>(i)] : i;
        const std::string_view text = dataset.row_text(row);

        if (format == OutputFormat::JsonLines) {
            buffer.push_back('{');
            for (std::size_t c = 0; c < targets.size(); ++c) {
                if (c > 0) buffer.push_back(',');
                append_json_string(buffer, dataset.column_name(targets[c]));
                buffer.push_back(':');
                const std::string value = dataset.decode_cell(row, targets[c]);
                const ColumnType type = dataset.column_type(targets[c]);
                if (value.empty()) {
                    buffer.append("null");
                } else if (is_numeric(type)) {
                    buffer.append(value);
                } else if (type == ColumnType::Boolean) {
                    bool b = false;
                    if (parse_bool(value, b)) buffer.append(b ? "true" : "false");
                    else append_json_string(buffer, value);
                } else {
                    append_json_string(buffer, value);
                }
            }
            buffer.append("}\n");
        } else {
            split_fields(text, dataset.dialect().delimiter, dataset.dialect().quote, spans);
            for (std::size_t c = 0; c < targets.size(); ++c) {
                if (c > 0) buffer.push_back(delimiter);
                if (targets[c] < spans.size()) {
                    append_raw_field(buffer, text, spans[targets[c]], delimiter, quote);
                }
            }
            buffer.push_back('\n');
        }

        ++stats.rows;
        if (buffer.size() >= kFlushThreshold) flush();
        if (failed) break;
    }
    flush();

    if (!to_stdout) {
        const bool close_ok = std::fclose(file) == 0;
        if (!close_ok) failed = true;
    } else {
        std::fflush(file);
    }
    if (failed) fail("export to '" + path + "' failed");
    if (options.progress) options.progress(1.0, "exporting");
    stats.seconds = now_seconds() - started;
    return stats;
}

void write_result(std::ostream& out, const query::ResultSet& result, OutputFormat format) {
    const char delimiter = format == OutputFormat::Tsv ? '\t' : ',';
    if (format == OutputFormat::JsonLines) {
        for (const auto& row : result.rows) {
            std::string line = "{";
            for (std::size_t c = 0; c < result.headers.size(); ++c) {
                if (c > 0) line.push_back(',');
                append_json_string(line, result.headers[c]);
                line.push_back(':');
                const std::string_view value = c < row.size() ? std::string_view(row[c]) : std::string_view();
                const bool numeric_column = c < result.types.size() && is_numeric(result.types[c]);
                if (value.empty()) {
                    line.append("null");
                } else if (numeric_column) {
                    line.append(value);
                } else {
                    append_json_string(line, value);
                }
            }
            line.append("}\n");
            out << line;
        }
        return;
    }

    std::string line;
    for (std::size_t c = 0; c < result.headers.size(); ++c) {
        if (c > 0) line.push_back(delimiter);
        append_csv_field(line, result.headers[c], delimiter, '"');
    }
    line.push_back('\n');
    out << line;
    for (const auto& row : result.rows) {
        line.clear();
        for (std::size_t c = 0; c < result.headers.size(); ++c) {
            if (c > 0) line.push_back(delimiter);
            append_csv_field(line, c < row.size() ? std::string_view(row[c]) : std::string_view(), delimiter, '"');
        }
        line.push_back('\n');
        out << line;
    }
}

} // namespace tess::io
