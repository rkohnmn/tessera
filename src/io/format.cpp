// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · format.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "io/format.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(TESSERA_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <io.h>
#  include <windows.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace tess::io {
namespace {

struct Ansi {
    bool enabled = false;

    [[nodiscard]] std::string wrap(std::string_view text, std::string_view code) const {
        if (!enabled) return std::string(text);
        std::string out = "\x1b[";
        out += code;
        out += 'm';
        out.append(text);
        out += "\x1b[0m";
        return out;
    }
    [[nodiscard]] std::string bold(std::string_view t) const { return wrap(t, "1"); }
    [[nodiscard]] std::string dim(std::string_view t) const { return wrap(t, "2"); }
    [[nodiscard]] std::string accent(std::string_view t) const { return wrap(t, "38;5;45"); }
    [[nodiscard]] std::string numeric(std::string_view t) const { return wrap(t, "38;5;114"); }
    [[nodiscard]] std::string temporal(std::string_view t) const { return wrap(t, "38;5;110"); }
    [[nodiscard]] std::string boolean(std::string_view t) const { return wrap(t, "38;5;176"); }
    [[nodiscard]] std::string muted(std::string_view t) const { return wrap(t, "38;5;244"); }
};

std::string pad_to(std::string_view text, int width, bool right_align) {
    const int visible = text_width(text);
    if (visible >= width) return std::string(text);
    std::string out;
    out.reserve(text.size() + static_cast<std::size_t>(width - visible));
    if (right_align) out.append(static_cast<std::size_t>(width - visible), ' ');
    out.append(text);
    if (!right_align) out.append(static_cast<std::size_t>(width - visible), ' ');
    return out;
}

constexpr std::string_view kSparkTicks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

} // namespace

bool stdout_is_tty() noexcept {
#if defined(TESSERA_WINDOWS)
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

int terminal_width() noexcept {
#if defined(TESSERA_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        return static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
    }
    return 100;
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return static_cast<int>(ws.ws_col);
    return 100;
#endif
}

bool colors_enabled() noexcept {
    if (std::getenv("NO_COLOR") != nullptr) return false;
    if (std::getenv("CLICOLOR_FORCE") != nullptr) return true;
    if (!stdout_is_tty()) return false;
#if defined(TESSERA_WINDOWS)
    // Windows 10+ terminal supports VT sequences once enabled; assume yes and
    // let the TUI turn it on explicitly.
    return true;
#else
    const char* term = std::getenv("TERM");
    return term != nullptr && std::string_view(term) != "dumb";
#endif
}

std::string type_label(ColumnType type) {
    return std::string(to_string(type));
}

std::string sparkline(const std::vector<Bucket>& buckets, std::size_t width) {
    if (buckets.empty()) return {};
    std::uint64_t peak = 0;
    for (const auto& bucket : buckets) peak = std::max(peak, bucket.count);
    if (peak == 0) return std::string(std::min(width, buckets.size()), ' ');

    // Resample to the requested width so the shape survives narrow terminals.
    std::string out;
    out.reserve(width);
    for (std::size_t i = 0; i < width; ++i) {
        const std::size_t from = i * buckets.size() / width;
        const std::size_t to = std::max(from + 1, (i + 1) * buckets.size() / width);
        std::uint64_t peak_in_slot = 0;
        for (std::size_t b = from; b < to && b < buckets.size(); ++b) {
            peak_in_slot = std::max(peak_in_slot, buckets[b].count);
        }
        if (peak_in_slot == 0) {
            out.append(" ");
            continue;
        }
        const double scaled = static_cast<double>(peak_in_slot) / static_cast<double>(peak);
        std::size_t level = static_cast<std::size_t>(scaled * 7.0 + 0.5);
        level = std::min<std::size_t>(level, 7);
        out.append(kSparkTicks[level]);
    }
    return out;
}

std::string render_table(const query::ResultSet& result, const TableOptions& options) {
    Ansi ansi{options.color};
    if (result.headers.empty()) return std::string();
    const std::size_t column_count = result.headers.size();

    std::size_t row_limit = result.rows.size();
    if (options.max_rows > 0) row_limit = std::min(row_limit, options.max_rows);

    std::vector<int> widths(column_count, 0);
    for (std::size_t c = 0; c < column_count; ++c) {
        widths[c] = static_cast<int>(std::min<std::size_t>(text_width(result.headers[c]), options.max_cell_width));
        widths[c] = std::max(widths[c], 1);
    }
    for (std::size_t r = 0; r < row_limit; ++r) {
        const auto& row = result.rows[r];
        for (std::size_t c = 0; c < column_count && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], static_cast<int>(std::min<std::size_t>(text_width(row[c]),
                                                                                   options.max_cell_width)));
        }
    }

    // Fit the table to the terminal: shrink the widest columns first.
    const int available = options.width > 0 ? options.width : terminal_width();
    int total = static_cast<int>(column_count) * 3 + 1;
    for (const int w : widths) total += w;
    while (total > available - 1) {
        int widest = 0;
        for (std::size_t c = 1; c < column_count; ++c) {
            if (widths[c] > widths[widest]) widest = static_cast<int>(c);
        }
        if (widths[widest] <= 6) break;
        --widths[widest];
        --total;
    }

    const bool box = options.unicode;
    const std::string horizontal = box ? "─" : "-";
    const auto rule = [&](std::string_view left, std::string_view mid, std::string_view right) {
        std::string out(left);
        for (std::size_t c = 0; c < column_count; ++c) {
            if (c > 0) out += mid;
            for (int i = 0; i < widths[c] + 2; ++i) out += horizontal;
        }
        out += right;
        out += '\n';
        return out;
    };

    std::string out;
    out.reserve((row_limit + 4) * (static_cast<std::size_t>(available) + 8));
    const std::string vertical = box ? "│" : "|";
    if (box) out += rule("┌", "┬", "┐");
    else out += rule("+", "+", "+");

    const auto emit_row = [&](const std::vector<std::string>& cells, bool is_header) {
        out += vertical;
        for (std::size_t c = 0; c < column_count; ++c) {
            const std::string_view raw = c < cells.size() ? std::string_view(cells[c]) : std::string_view();
            const std::string clipped = truncate_width(raw, widths[c]);
            const bool right = !is_header && c < result.types.size() && is_numeric(result.types[c]);
            std::string painted;
            if (is_header) {
                painted = ansi.bold(pad_to(clipped, widths[c], false));
            } else {
                std::string padded = pad_to(clipped, widths[c], right);
                if (ansi.enabled && c < result.types.size()) {
                    switch (result.types[c]) {
                        case ColumnType::Integer:
                        case ColumnType::Float: painted = ansi.numeric(padded); break;
                        case ColumnType::Timestamp: painted = ansi.temporal(padded); break;
                        case ColumnType::Boolean: painted = ansi.boolean(padded); break;
                        default: painted = padded; break;
                    }
                } else {
                    painted = padded;
                }
            }
            out += ' ';
            out += painted;
            out += ' ';
            out += vertical;
        }
        out += '\n';
    };

    if (options.header) {
        emit_row(result.headers, true);
        out += rule(box ? "├" : "+", box ? "┼" : "+", box ? "┤" : "+");
    }
    for (std::size_t r = 0; r < row_limit; ++r) emit_row(result.rows[r], false);
    if (row_limit == 0) {
        out += vertical;
        for (std::size_t c = 0; c < column_count; ++c) {
            const std::string empty = pad_to("", widths[c], false);
            out += ' ';
            out += empty;
            out += ' ';
            out += vertical;
        }
        out += '\n';
    }
    out += rule(box ? "└" : "+", box ? "┴" : "+", box ? "┘" : "+");
    return out;
}

std::string render_info(const Dataset& dataset, const TableOptions& options) {
    Ansi ansi{options.color};
    const DatasetInfo& info = dataset.info();
    std::string out;

    out += ansi.accent("◆ ") + ansi.bold(info.name) + "\n";
    const auto line = [&](std::string_view label, const std::string& value) {
        out += "  " + ansi.muted(pad_to(label, 14, false)) + value + "\n";
    };

    const bool single_column = dataset.dialect().format == Format::Delimited && dataset.dialect().delimiter == 0;
    line("format", std::string(to_string(info.format)) + " · " + dataset.dialect().delimiter_name() +
                        (single_column ? "" : " separated") +
                        (info.has_header ? " · header row" : " · no header"));
    line("encoding", std::string(to_string(info.encoding)) + (info.crlf ? " · CRLF line endings" : ""));
    line("size", format_bytes(info.bytes));
    line("rows", format_count(info.rows));
    line("columns", format_count(info.columns));
    if (info.rows > 0) {
        const double bytes_per_row = static_cast<double>(info.bytes) / static_cast<double>(info.rows);
        line("row width", format_bytes(static_cast<std::uint64_t>(bytes_per_row)) + " average · " +
                              format_bytes(info.widest_row_bytes) + " widest sampled");
    }
    if (info.null_cells_sampled > 0) {
        const std::uint64_t sampled = static_cast<std::uint64_t>(info.columns) * 4096ull;
        line("nulls", format_count(info.null_cells_sampled) + " in sampled cells");
        (void)sampled;
    }
    line("index", std::string(info.index_from_sidecar ? "cached sidecar" : "built at open") + " · " +
                      format_double(info.index_seconds, 3) + " s · " +
                      format_bytes(info.index_bytes) + " resident");
    line("open time", format_double(info.open_seconds * 1000.0, 1) + " ms");
    line("threads", std::to_string(std::max(1u, std::thread::hardware_concurrency())));

    if (!info.notes.empty()) {
        out += "\n  " + ansi.bold("detection") + "\n";
        for (const auto& note : info.notes) out += "    · " + note + "\n";
    }

    out += "\n  " + ansi.bold("columns") + "\n";
    for (std::uint32_t c = 0; c < dataset.column_count(); ++c) {
        const ColumnMeta& column = dataset.columns()[c];
        std::string label = pad_to(truncate_width(column.name, 28), 30, false);
        std::string type = pad_to(type_label(column.type), 10, false);
        if (ansi.enabled) {
            if (is_numeric(column.type)) type = ansi.numeric(type);
            else if (column.type == ColumnType::Timestamp) type = ansi.temporal(type);
            else if (column.type == ColumnType::Boolean) type = ansi.boolean(type);
            else type = ansi.muted(type);
        }
        std::string confidence = format_percent(column.type_confidence, 0);
        out += "    " + std::to_string(c + 1) + ". " + label + type + ansi.muted(confidence + " typed") + "\n";
    }
    return out;
}

std::string render_profiles(const std::vector<ColumnProfile>& profiles, const TableOptions& options,
                            std::uint64_t total_rows) {
    Ansi ansi{options.color};
    std::string out;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const ColumnProfile& p = profiles[i];
        if (i > 0) out += "\n";
        std::string header = ansi.accent("◆ ") + ansi.bold(p.name) + "  " + ansi.muted(type_label(p.type));
        out += header + "\n";

        const double fill = p.total > 0 ? 1.0 - static_cast<double>(p.nulls) / static_cast<double>(p.total) : 0.0;
        std::string summary = "  " + ansi.muted("fill ") + format_percent(fill, 1);
        summary += ansi.muted("  ·  distinct ") + format_count(p.distinct_estimate);
        if (p.distinct_is_estimate) summary += ansi.muted("~");
        summary += ansi.muted("  ·  nulls ") + format_count(p.nulls);
        if (total_rows > 0 && p.total != total_rows) {
            summary += ansi.muted("  ·  sampled from ") + format_count(p.total);
        }
        out += summary + "\n";

        if (p.numeric) {
            out += "  " + ansi.muted("min ") + format_double(p.min, 8) + ansi.muted("   p25 ") +
                   format_double(p.p25, 8) + ansi.muted("   median ") + format_double(p.median, 8) + ansi.muted("   p75 ") +
                   format_double(p.p75, 8) + ansi.muted("   max ") + format_double(p.max, 8) + "\n";
            out += "  " + ansi.muted("mean ") + format_double(p.mean, 8) + ansi.muted("   sd ") +
                   format_double(p.stddev, 8) + ansi.muted("   p99 ") + format_double(p.p99, 8) + "\n";
        } else if (p.temporal) {
            out += "  " + ansi.muted("from ") + ansi.temporal(format_timestamp(p.time_min)) + ansi.muted("   to ") +
                   ansi.temporal(format_timestamp(p.time_max)) + "\n";
        } else {
            out += "  " + ansi.muted("length ") + format_int(static_cast<std::int64_t>(p.min_length)) + "–" +
                   format_int(static_cast<std::int64_t>(p.max_length)) + ansi.muted("   mean ") +
                   format_double(p.mean_length, 3) + "\n";
        }

        if (!p.histogram.empty()) {
            std::uint64_t peak = 0;
            for (const auto& bucket : p.histogram) peak = std::max(peak, bucket.count);
            if (peak > 0) {
                const int width = std::max(20, std::min(options.width > 0 ? options.width - 20 : 56, 72));
                out += "  " + ansi.accent(sparkline(p.histogram, static_cast<std::size_t>(width))) + "\n";
            }
        }

        if (!p.top_values.empty()) {
            out += "  " + ansi.muted("top ");
            for (std::size_t v = 0; v < p.top_values.size(); ++v) {
                if (v > 0) out += ansi.muted("  ·  ");
                const std::string value = truncate_width(p.top_values[v].first, 28);
                out += value + ansi.muted(" (" + format_count(p.top_values[v].second) + ")");
            }
            out += "\n";
        }
    }
    return out;
}

} // namespace tess::io
