// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · commands.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "cli/commands.hpp"

#include "common/error.hpp"
#include "common/util.hpp"
#include "core/dataset.hpp"
#include "core/dialect.hpp"
#include "core/indexer.hpp"
#include "core/profile.hpp"
#include "io/format.hpp"
#include "io/writer.hpp"
#include "query/exec.hpp"
#include "tui/app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#if defined(TESSERA_WINDOWS)
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace tess::cli {
namespace {

// ── Small terminal helpers ──────────────────────────────────────────────────
bool stderr_is_tty() noexcept {
#if defined(TESSERA_WINDOWS)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

class ProgressLine {
public:
    ProgressLine() : enabled_(stderr_is_tty() && std::getenv("TESSERA_NO_PROGRESS") == nullptr) {}
    ~ProgressLine() { clear(); }

    [[nodiscard]] ProgressFn callback() {
        return [this](double fraction, std::string_view phase) { update(fraction, phase); };
    }

    void update(double fraction, std::string_view phase) {
        if (!enabled_) return;
        if (fraction < last_ + 0.01 && fraction < 0.999) return;
        last_ = fraction;
        const int percent = static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 100.0);
        std::fprintf(stderr, "\r\x1b[2K  %-18.*s %3d%%", static_cast<int>(phase.size()), phase.data(), percent);
        std::fflush(stderr);
        printed_ = true;
    }

    void clear() {
        if (enabled_ && printed_) {
            std::fprintf(stderr, "\r\x1b[2K");
            std::fflush(stderr);
        }
        printed_ = false;
    }

private:
    bool enabled_ = false;
    bool printed_ = false;
    double last_ = -1.0;
};

// ── Argument parsing ────────────────────────────────────────────────────────
struct Options {
    std::vector<std::string> positional;
    std::string sql;
    std::string output;
    std::string format;
    std::vector<std::string> columns;
    std::optional<std::int64_t> limit;
    std::int64_t offset = 0;
    std::optional<bool> header;
    bool no_index = false;
    bool cache_index = false;
    bool no_color = false;
    bool quiet = false;
    bool sample = false;
    std::optional<char> delimiter;
    unsigned threads = 0;
    bool help = false;
    bool version = false;
};

[[noreturn]] void usage_error(const std::string& message) {
    throw Error(message + "\nRun 'tessera help' for usage.");
}

std::int64_t parse_number_arg(std::string_view value, std::string_view flag) {
    std::int64_t number = 0;
    if (!parse_int(value, number) || number < 0) {
        usage_error(std::string(flag) + " expects a non-negative whole number, got '" + std::string(value) + "'");
    }
    return number;
}

Options parse_options(const std::vector<std::string>& args) {
    Options options;
    bool positional_only = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (positional_only || arg.empty() || arg[0] != '-' || arg == "-") {
            options.positional.push_back(arg);
            continue;
        }
        if (arg == "--") {
            positional_only = true;
            continue;
        }

        std::string name = arg;
        std::string inline_value;
        const std::size_t equals = name.find('=');
        if (equals != std::string::npos) {
            inline_value = name.substr(equals + 1);
            name = name.substr(0, equals);
        }
        const auto value_of = [&](std::string_view flag) -> std::string {
            if (!inline_value.empty()) return inline_value;
            if (i + 1 >= args.size()) usage_error(std::string(flag) + " requires a value");
            return args[++i];
        };

        if (name == "-h" || name == "--help") options.help = true;
        else if (name == "--version" || name == "-V") options.version = true;
        else if (name == "--no-color") options.no_color = true;
        else if (name == "--quiet") options.quiet = true;
        else if (name == "--no-header") options.header = false;
        else if (name == "--header") options.header = true;
        else if (name == "--no-index") options.no_index = true;
        else if (name == "--cache-index") options.cache_index = true;
        else if (name == "--sample") options.sample = true;
        else if (name == "-n" || name == "--limit" || name == "--rows") {
            options.limit = parse_number_arg(value_of(name), name);
        } else if (name == "--offset") {
            options.offset = parse_number_arg(value_of(name), name);
        } else if (name == "-c" || name == "--columns") {
            const std::string column_list = value_of(name);
            for (auto part : split(column_list, ',')) {
                const std::string_view trimmed = trim(part);
                if (!trimmed.empty()) options.columns.emplace_back(trimmed);
            }
        } else if (name == "-o" || name == "--output") {
            options.output = value_of(name);
        } else if (name == "-f" || name == "--format") {
            options.format = value_of(name);
        } else if (name == "-q" || name == "--query") {
            options.sql = value_of(name);
        } else if (name == "-d" || name == "--delimiter") {
            const std::string value = value_of(name);
            if (value == "tab" || value == "\\t") options.delimiter = '\t';
            else if (value == "pipe") options.delimiter = '|';
            else if (value == "semicolon") options.delimiter = ';';
            else if (value == "comma") options.delimiter = ',';
            else if (value.size() == 1) options.delimiter = value[0];
            else usage_error("--delimiter expects a single character (or tab/pipe/semicolon/comma)");
        } else if (name == "--threads") {
            options.threads = static_cast<unsigned>(parse_number_arg(value_of(name), name));
        } else {
            usage_error("unknown option '" + arg + "'");
        }
    }
    return options;
}

// ── Shared plumbing ─────────────────────────────────────────────────────────
struct AppContext {
    Options options;
    ProgressLine progress;
};

OpenOptions open_options(const AppContext& context) {
    OpenOptions options;
    options.use_sidecar = !context.options.no_index;
    options.write_sidecar = context.options.cache_index;
    options.delimiter = context.options.delimiter;
    options.header = context.options.header;
    options.threads = context.options.threads;
    return options;
}

std::shared_ptr<Dataset> open_dataset(AppContext& context, const std::string& path) {
    if (path.empty()) usage_error("missing input file");
    if (!file_exists(path)) {
        throw Error("no such file: '" + path + "'");
    }
    OpenOptions options = open_options(context);
    ProgressFn progress = context.progress.callback();
    options.progress = progress;
    auto dataset = [&] {
        try {
            return Dataset::open(path, options);
        } catch (const Error&) {
            context.progress.clear();
            throw;
        }
    }();
    context.progress.clear();
    return dataset;
}

std::vector<std::uint32_t> resolve_columns(const Dataset& dataset, const std::vector<std::string>& names) {
    std::vector<std::uint32_t> columns;
    for (const auto& name : names) {
        const auto index = dataset.column_index(name);
        if (!index.has_value()) {
            std::string available;
            for (std::uint32_t c = 0; c < dataset.column_count() && c < 12; ++c) {
                if (c) available += ", ";
                available += dataset.column_name(c);
            }
            if (dataset.column_count() > 12) available += ", …";
            throw Error("no column named '" + name + "'. Available: " + available);
        }
        columns.push_back(*index);
    }
    return columns;
}

bool interactive_output() { return io::stdout_is_tty(); }


io::TableOptions table_options(const Options& options, bool /*table_allowed*/) {
    io::TableOptions table;
    table.color = !options.no_color && io::colors_enabled();
    table.unicode = true;
    table.width = io::terminal_width();
    return table;
}

/// Prints either a table (terminal) or machine-readable records (pipe).
void emit_result(const Dataset& dataset, query::ResultSet& result, const Options& options) {
    const bool want_table = options.format.empty() ? interactive_output()
                                                  : iequals(options.format, "table");
    if (want_table) {
        std::string text = io::render_table(result, table_options(options, true));
        std::fputs(text.c_str(), stdout);
        if (!options.quiet) {
            std::string footer = "  " + format_count(result.rows.size()) + " row" +
                                 (result.rows.size() == 1 ? "" : "s");
            if (result.truncated) footer += " (truncated)";
            footer += " · " + format_count(result.scanned) + " scanned · " +
                      format_double(result.seconds * 1000.0, 1) + " ms";
            if (io::colors_enabled() && !options.no_color) {
                std::fprintf(stdout, "\x1b[38;5;244m%s\x1b[0m\n", footer.c_str());
            } else {
                std::fprintf(stdout, "%s\n", footer.c_str());
            }
        }
        (void)dataset;
        return;
    }
    const io::OutputFormat format = options.format.empty() ? io::OutputFormat::Csv
                                                           : io::parse_output_format(options.format);
    io::write_result(std::cout, result, format);
    std::cout.flush();
}

// ── Commands ────────────────────────────────────────────────────────────────
int command_info(AppContext& context) {
    if (context.options.positional.size() < 2) usage_error("info needs at least one file");
    for (std::size_t i = 1; i < context.options.positional.size(); ++i) {
        if (i > 1) std::fputs("\n", stdout);
        auto dataset = open_dataset(context, context.options.positional[i]);
        const std::string text = io::render_info(*dataset, table_options(context.options, false));
        std::fputs(text.c_str(), stdout);
    }
    return 0;
}

int command_rows(AppContext& context, bool from_end) {
    if (context.options.positional.size() < 2) usage_error("head/tail needs a file");
    auto dataset = open_dataset(context, context.options.positional[1]);
    const std::uint64_t count = context.options.limit.value_or(10) < 0
                                    ? 10
                                    : static_cast<std::uint64_t>(context.options.limit.value_or(10));
    const std::vector<std::uint32_t> columns =
        context.options.columns.empty() ? std::vector<std::uint32_t>{}
                                        : resolve_columns(*dataset, context.options.columns);

    std::vector<std::uint32_t> targets = columns;
    if (targets.empty()) {
        for (std::uint32_t c = 0; c < dataset->column_count(); ++c) targets.push_back(c);
    }

    query::ResultSet result;
    for (const std::uint32_t column : targets) {
        result.headers.emplace_back(dataset->column_name(column));
        result.types.push_back(dataset->column_type(column));
    }
    const std::uint64_t total = dataset->row_count();
    const std::uint64_t start = from_end ? (total > count ? total - count : 0) : 0;
    const std::uint64_t end = from_end ? total : std::min<std::uint64_t>(total, count);
    for (std::uint64_t row = start; row < end; ++row) {
        std::vector<std::string> cells;
        cells.reserve(targets.size());
        for (const std::uint32_t column : targets) cells.push_back(dataset->decode_cell(row, column));
        result.rows.push_back(std::move(cells));
    }
    result.scanned = total;
    result.matched = result.rows.size();
    result.truncated = !from_end && total > end;
    emit_result(*dataset, result, context.options);
    return 0;
}

int command_stats(AppContext& context) {
    if (context.options.positional.size() < 2) usage_error("stats needs a file");
    auto dataset = open_dataset(context, context.options.positional[1]);
    const std::vector<std::uint32_t> columns =
        context.options.columns.empty() ? std::vector<std::uint32_t>{}
                                        : resolve_columns(*dataset, context.options.columns);
    ProfileOptions options;
    options.progress = context.progress.callback();
    if (context.options.sample) options.sample_limit = 200000;
    auto profiles = profile_columns(*dataset, columns, options);
    context.progress.clear();

    if (interactive_output() && context.options.format.empty()) {
        std::string header = "◆ " + dataset->info().name + "  ";
        header += format_count(dataset->row_count()) + " rows × " + format_count(dataset->column_count()) + " columns\n\n";
        std::fputs(header.c_str(), stdout);
        const std::string text = io::render_profiles(profiles, table_options(context.options, true), dataset->row_count());
        std::fputs(text.c_str(), stdout);
        return 0;
    }

    // Pipe-friendly summary.
    std::printf("column,type,rows,nulls,distinct,min,max,mean,stddev\n");
    for (const auto& p : profiles) {
        std::printf("%s,%s,%llu,%llu,%llu", p.name.c_str(), std::string(to_string(p.type)).c_str(),
                    static_cast<unsigned long long>(p.total), static_cast<unsigned long long>(p.nulls),
                    static_cast<unsigned long long>(p.distinct_estimate));
        if (p.numeric) {
            std::printf(",%.10g,%.10g,%.10g,%.10g\n", p.min, p.max, p.mean, p.stddev);
        } else if (p.temporal) {
            std::printf(",%s,%s,,\n", format_timestamp(p.time_min).c_str(), format_timestamp(p.time_max).c_str());
        } else {
            std::printf(",,,,\n");
        }
    }
    return 0;
}

int command_query(AppContext& context) {
    if (context.options.positional.size() < 3) {
        usage_error("query needs a file and a statement, e.g. tessera query sales.csv \"SELECT region, count(*) FROM t GROUP BY region\"");
    }
    auto dataset = open_dataset(context, context.options.positional[1]);
    std::string sql = context.options.sql;
    if (sql.empty()) {
        for (std::size_t i = 2; i < context.options.positional.size(); ++i) {
            if (!sql.empty()) sql.push_back(' ');
            sql += context.options.positional[i];
        }
    }
    std::string error;
    auto query = query::parse_query(sql, *dataset, error);
    if (!query.has_value()) throw Error("query error: " + error);

    query::RunOptions run;
    run.cancel = nullptr;
    run.progress = context.progress.callback();
    if (context.options.limit.has_value()) query->limit = static_cast<std::uint64_t>(*context.options.limit);
    auto result = query::execute(*dataset, *query, run);
    context.progress.clear();
    emit_result(*dataset, result, context.options);
    return 0;
}

int command_export(AppContext& context) {
    if (context.options.positional.size() < 2) usage_error("export needs a file");
    auto dataset = open_dataset(context, context.options.positional[1]);
    const io::OutputFormat format =
        context.options.format.empty() ? io::OutputFormat::Csv : io::parse_output_format(context.options.format);
    const std::string output = context.options.output.empty() ? std::string("-") : context.options.output;

    query::RunOptions run;
    run.progress = context.progress.callback();

    if (!context.options.sql.empty()) {
        std::string error;
        auto query = query::parse_query(context.options.sql, *dataset, error);
        if (!query.has_value()) throw Error("query error: " + error);
        auto result = query::execute(*dataset, *query, run);
        context.progress.clear();
        if (output == "-") {
            io::write_result(std::cout, result, format);
            std::cout.flush();
        } else {
            std::FILE* file = fopen_utf8(output, "wb");
            if (file == nullptr) throw Error("cannot create '" + output + "'");
            std::string payload;
            if (format == io::OutputFormat::Csv || format == io::OutputFormat::Tsv) {
                const char delimiter = format == io::OutputFormat::Tsv ? '\t' : ',';
                std::string line;
                for (std::size_t c = 0; c < result.headers.size(); ++c) {
                    if (c) line.push_back(delimiter);
                    append_csv_field(line, result.headers[c], delimiter, '"');
                }
                line.push_back('\n');
                payload += line;
                for (const auto& row : result.rows) {
                    line.clear();
                    for (std::size_t c = 0; c < result.headers.size(); ++c) {
                        if (c) line.push_back(delimiter);
                        append_csv_field(line, c < row.size() ? std::string_view(row[c]) : std::string_view(),
                                         delimiter, '"');
                    }
                    line.push_back('\n');
                    payload += line;
                }
            } else {
                for (const auto& row : result.rows) {
                    payload.push_back('{');
                    for (std::size_t c = 0; c < result.headers.size(); ++c) {
                        if (c) payload.push_back(',');
                        append_json_string(payload, result.headers[c]);
                        payload.push_back(':');
                        const std::string_view value = c < row.size() ? std::string_view(row[c]) : std::string_view();
                        if (value.empty()) payload.append("null");
                        else if (c < result.types.size() && is_numeric(result.types[c])) payload.append(value);
                        else append_json_string(payload, value);
                    }
                    payload.append("}\n");
                }
            }
            std::fwrite(payload.data(), 1, payload.size(), file);
            std::fclose(file);
            if (!context.options.quiet) {
                std::fprintf(stderr, "wrote %s · %s · %s\n", output.c_str(), format_bytes(payload.size()).c_str(),
                             format_count(result.rows.size()).c_str());
            }
        }
        return 0;
    }

    const std::vector<std::uint32_t> columns =
        context.options.columns.empty() ? std::vector<std::uint32_t>{}
                                        : resolve_columns(*dataset, context.options.columns);
    const auto stats = io::export_rows(*dataset, nullptr, columns, output, format, run);
    context.progress.clear();
    if (!context.options.quiet) {
        std::fprintf(stderr, "wrote %s · %s rows · %s in %.2f s\n", output.c_str(),
                     format_count(stats.rows).c_str(), format_bytes(stats.bytes).c_str(), stats.seconds);
    }
    return 0;
}

int command_index(AppContext& context) {
    if (context.options.positional.size() < 2) usage_error("index needs a file");
    std::string path = context.options.positional[1];
    OpenOptions options = open_options(context);
    options.use_sidecar = false;
    options.write_sidecar = false;
    ProgressLine progress;
    options.progress = progress.callback();
    auto dataset = Dataset::open(path, options);
    progress.clear();
    const std::uint64_t bytes = dataset->save_index_cache();
    if (bytes == 0) {
        throw Error("could not write the index cache next to '" + path + "' (read-only directory?)");
    }
    std::printf("cached %s rows in %s\n", format_count(dataset->row_count()).c_str(),
                format_bytes(bytes).c_str());
    std::printf("next open of this file will skip the scan entirely.\n");
    return 0;
}

int command_bench(AppContext& context) {
    if (context.options.positional.size() < 2) usage_error("bench needs a file");
    const std::string path = context.options.positional[1];
    OpenOptions options;
    options.threads = context.options.threads;
    options.use_sidecar = false;
    ProgressLine progress;
    auto dataset = Dataset::open(path, options);
    const DatasetInfo& info = dataset->info();
    const std::uint64_t rows = dataset->row_count();
    const double megabytes = static_cast<double>(info.bytes) / (1024.0 * 1024.0);

    std::printf("◆ tessera %s benchmark\n", TESSERA_VERSION);
    std::printf("  file      %s (%s, %s rows × %s columns, %u threads)\n", info.name.c_str(),
                format_bytes(info.bytes).c_str(), format_count(rows).c_str(),
                format_count(dataset->column_count()).c_str(), default_thread_count(options.threads));
    std::printf("  index     %.3f s  ·  %.2f GB/s  ·  %.1f M rows/s  ·  %.1f MB resident\n", info.index_seconds,
                info.index_seconds > 0 ? (static_cast<double>(info.bytes) / 1e9) / info.index_seconds : 0.0,
                info.index_seconds > 0 ? static_cast<double>(rows) / info.index_seconds / 1e6 : 0.0,
                static_cast<double>(info.index_bytes) / 1024.0 / 1024.0);

    // Sequential scan: read every cell of every column, no projection shortcuts.
    std::int64_t checksum = 0;
    double start = now_seconds();
    for (std::uint64_t row = 0; row < rows; ++row) {
        const std::string_view text = dataset->row_text(row);
        checksum += static_cast<std::int64_t>(text.size());
        if ((row & 65535u) == 0) progress.update(static_cast<double>(row) / std::max<std::uint64_t>(1, rows), "scanning");
    }
    const double scan_seconds = now_seconds() - start;
    progress.clear();
    std::printf("  scan      %.3f s  ·  %.2f GB/s  ·  %.1f M rows/s\n", scan_seconds,
                scan_seconds > 0 ? megabytes / 1024.0 / scan_seconds : 0.0,
                scan_seconds > 0 ? static_cast<double>(rows) / scan_seconds / 1e6 : 0.0);

    // Filter: first numeric column above its midpoint, or a substring search.
    std::uint32_t numeric_column = dataset->column_count();
    for (std::uint32_t c = 0; c < dataset->column_count(); ++c) {
        if (is_numeric(dataset->column_type(c))) {
            numeric_column = c;
            break;
        }
    }
    if (numeric_column < dataset->column_count() && rows > 0) {
        const auto middle = dataset->cell_double(rows / 2, numeric_column).value_or(0.0);
        const std::string expression = std::string(dataset->column_name(numeric_column)) + " > " +
                                       format_double(middle, 12);
        std::string error;
        auto predicate = query::parse_expression(expression, *dataset, error);
        if (predicate) {
            query::RunOptions run;
            double seconds = 0.0;
            const auto matched = query::filter_rows(*dataset, *predicate, run, &seconds);
            std::printf("  filter    %.3f s  ·  %.1f M rows/s  ·  %s matched  [%s]\n", seconds,
                        seconds > 0 ? static_cast<double>(rows) / seconds / 1e6 : 0.0,
                        format_count(matched.size()).c_str(), expression.c_str());
        }
    }

    // Sort a bounded slice so the benchmark stays interactive on huge files.
    std::uint32_t sort_column = dataset->column_count() > 1 ? 1 : 0;
    if (rows > 1 && dataset->column_count() > 0) {
        std::vector<std::uint64_t> sample;
        const std::uint64_t take = std::min<std::uint64_t>(rows, 1000000);
        sample.reserve(static_cast<std::size_t>(take));
        for (std::uint64_t i = 0; i < take; ++i) sample.push_back(i);
        std::vector<query::OrderKey> keys;
        query::OrderKey key;
        key.expr = query::Expr::column_ref(sort_column, std::string(dataset->column_name(sort_column)));
        keys.push_back(std::move(key));
        query::RunOptions run;
        const double sort_start = now_seconds();
        query::sort_rows(*dataset, sample, keys, run);
        const double sort_seconds = now_seconds() - sort_start;
        std::printf("  sort      %.3f s  ·  %s rows by '%s'  ·  %.1f M rows/s\n", sort_seconds,
                    format_count(sample.size()).c_str(), std::string(dataset->column_name(sort_column)).c_str(),
                    sort_seconds > 0 ? static_cast<double>(sample.size()) / sort_seconds / 1e6 : 0.0);
    }

    std::printf("  checksum  %lld (keeps the scan honest)\n", static_cast<long long>(checksum));
    return 0;
}

int command_view(AppContext& context) {
    if (context.options.positional.empty()) usage_error("view needs a file");
    auto dataset = open_dataset(context, context.options.positional.front());
    return tui::run(dataset);
}

// ── Help ────────────────────────────────────────────────────────────────────
std::string version_banner() {
    return std::string("tessera ") + TESSERA_VERSION;
}

void print_usage() {
    const bool color = io::colors_enabled();
    const auto bold = [&](std::string_view text) {
        return color ? "\x1b[1m" + std::string(text) + "\x1b[0m" : std::string(text);
    };
    const auto dim = [&](std::string_view text) {
        return color ? "\x1b[38;5;244m" + std::string(text) + "\x1b[0m" : std::string(text);
    };
    const auto accent = [&](std::string_view text) {
        return color ? "\x1b[38;5;45m" + std::string(text) + "\x1b[0m" : std::string(text);
    };

    std::fputs("\n", stdout);
    std::printf("  %s %s\n", accent("◆ tessera").c_str(),
                dim((std::string(TESSERA_VERSION) + " · instant, local, offline").c_str()).c_str());
    std::printf("  %s\n\n", dim("Explore multi-gigabyte CSV, TSV and JSONL files without loading them.").c_str());

    std::printf("  %s\n", bold("USAGE").c_str());
    std::printf("    tessera <file>                     open the interactive explorer\n");
    std::printf("    tessera <command> <file> [options]\n\n");

    std::printf("  %s\n", bold("COMMANDS").c_str());
    std::printf("    %s <file>                   file shape, detection notes, column types\n", accent("info").c_str());
    std::printf("    %s <file> [-n 20]            first rows\n", accent("head").c_str());
    std::printf("    %s <file> [-n 20]            last rows\n", accent("tail").c_str());
    std::printf("    %s <file> [-c a,b]           column profiles, quantiles, distribution\n", accent("stats").c_str());
    std::printf("    %s <file> \"SELECT ...\"       run a query, print a table or CSV\n", accent("query").c_str());
    std::printf("    %s <file> [-q SQL] [-o out]  stream rows to disk\n", accent("export").c_str());
    std::printf("    %s <file>                    build a .tessera.idx cache\n", accent("index").c_str());
    std::printf("    %s <file>                    measure open, scan, filter and sort\n", accent("bench").c_str());
    std::printf("    %s                          this message\n", accent("help").c_str());
    std::printf("    %s                       version\n\n", accent("version").c_str());

    std::printf("  %s\n", bold("OPTIONS").c_str());
    std::printf("    -n, --limit <n>       row limit for head/query\n");
    std::printf("    -c, --columns <a,b>   restrict to columns\n");
    std::printf("    -q, --query <sql>     statement for query/export\n");
    std::printf("    -o, --output <path>   export destination ('-' for stdout)\n");
    std::printf("    -f, --format <fmt>    csv | tsv | jsonl | table\n");
    std::printf("    -d, --delimiter <c>   override delimiter detection\n");
    std::printf("        --header          force a header row\n");
    std::printf("        --no-header       force no header row\n");
    std::printf("        --threads <n>     indexer worker count\n");
    std::printf("        --cache-index     write a .tessera.idx next to the file\n");
    std::printf("        --no-index        ignore an existing .tessera.idx\n");
    std::printf("        --sample          profile a sample instead of every row\n");
    std::printf("        --no-color        disable colour\n\n");

    std::printf("  %s\n", bold("EXAMPLES").c_str());
    std::printf("    %s\n", dim("tessera sales_2024.csv").c_str());
    std::printf("    %s\n", dim("tessera query orders.csv \"SELECT region, count(*) n, sum(total) revenue\\\n"
                                "                              FROM t GROUP BY region ORDER BY revenue DESC\"").c_str());
    std::printf("    %s\n", dim("tessera stats events.jsonl --sample").c_str());
    std::printf("    %s\n\n", dim("tessera export huge.csv -q \"SELECT * WHERE status = 'failed'\" -o failed.csv").c_str());
}

int dispatch(AppContext& context) {
    if (context.options.help) {
        print_usage();
        return 0;
    }
    if (context.options.version) {
        std::printf("%s\n", version_banner().c_str());
        return 0;
    }
    if (context.options.positional.empty()) {
        print_usage();
        return 0;
    }

    const std::string& command = context.options.positional[0];
    if (command == "help") {
        print_usage();
        return 0;
    }
    if (command == "version") {
        std::printf("%s\n", version_banner().c_str());
        return 0;
    }
    if (command == "info") return command_info(context);
    if (command == "head") return command_rows(context, false);
    if (command == "tail") return command_rows(context, true);
    if (command == "stats") return command_stats(context);
    if (command == "query") return command_query(context);
    if (command == "export") return command_export(context);
    if (command == "index") return command_index(context);
    if (command == "bench") return command_bench(context);
    if (command == "view" || command == "open" || command == "tui") return command_view(context);

    // Bare file argument: open the explorer.
    if (file_exists(command)) return command_view(context);

    throw Error("unknown command or file: '" + command + "'\nRun 'tessera help' for usage.");
}

} // namespace

int run(const std::vector<std::string>& args) {
    configure_console();
    AppContext context;
    // Everything that can throw lives inside this frame. Option parsing in
    // particular reports bad flags by throwing, and it used to be called
    // outside the try, so `tessera --bogus` escaped to std::terminate and
    // aborted (exit 134 here, exit 3 on Windows) instead of printing one line.
    try {
        context.options = parse_options(args);
        return dispatch(context);
    } catch (const Canceled&) {
        context.progress.clear();
        std::fprintf(stderr, "cancelled\n");
        return 130;
    } catch (const Error& error) {
        context.progress.clear();
        const bool color = io::colors_enabled();
        std::fprintf(stderr, "%s %s\n", color ? "\x1b[31merror\x1b[0m" : "error", error.what());
        return 1;
    }
}

} // namespace tess::cli
