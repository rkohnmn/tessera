// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · app.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "tui/app.hpp"

#include "common/util.hpp"
#include "io/writer.hpp"
#include "query/parser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace tess::tui {
namespace {

std::string encode_utf8(char32_t cp) {
    std::string out;
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
    return out;
}

int digit_count(std::uint64_t value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

std::size_t previous_rune(const std::string& text, std::size_t position) {
    if (position == 0) return 0;
    std::size_t index = position - 1;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) --index;
    return index;
}

std::size_t next_rune(const std::string& text, std::size_t position) {
    if (position >= text.size()) return text.size();
    const Rune rune = decode_rune(text, position);
    return std::min(text.size(), position + static_cast<std::size_t>(rune.length));
}

void prompt_insert(App& app, char32_t codepoint) {
    const std::string bytes = encode_utf8(codepoint);
    app.prompt.insert(app.prompt_cursor, bytes);
    app.prompt_cursor += bytes.size();
    app.prompt_error.clear();
}

void prompt_backspace(App& app) {
    if (app.prompt_cursor == 0) return;
    const std::size_t previous = previous_rune(app.prompt, app.prompt_cursor);
    app.prompt.erase(previous, app.prompt_cursor - previous);
    app.prompt_cursor = previous;
}

void prompt_delete(App& app) {
    if (app.prompt_cursor >= app.prompt.size()) return;
    const std::size_t next = next_rune(app.prompt, app.prompt_cursor);
    app.prompt.erase(app.prompt_cursor, next - app.prompt_cursor);
}

std::string default_export_name(const Dataset& dataset) {
    std::string name = dataset.info().name;
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    if (name.empty()) name = "tessera";
    return name + ".export.csv";
}

/// Splits "command rest of the line" without copying the rest twice.
std::pair<std::string, std::string> split_command(std::string_view text) {
    text = trim(text);
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos) return {std::string(text), {}};
    return {to_lower(text.substr(0, space)), std::string(trim(text.substr(space + 1)))};
}

} // namespace

// ── Construction ────────────────────────────────────────────────────────────
App::App(std::shared_ptr<Dataset> ds) : dataset(std::move(ds)) {
    started_at = now_seconds();
    compute_widths();
}

App::~App() { cancel_job(); }

void App::compute_widths() {
    widths.assign(dataset->column_count(), 6);
    for (std::uint32_t c = 0; c < dataset->column_count(); ++c) {
        const ColumnMeta& meta = dataset->columns()[c];
        // One column of inner padding, one for the type tag, one gap before the
        // next separator; the header must never be the thing that gets clipped.
        int width = static_cast<int>(text_width(meta.name)) + 3;
        width = std::max(width, static_cast<int>(meta.sample_width) + 2);
        if (is_numeric(meta.type)) width = std::max(width, 8);
        widths[c] = std::clamp(width, 6, 40);
    }
}

std::uint64_t App::visible_count() const {
    return identity_view ? dataset->row_count() : rows.size();
}

std::uint64_t App::row_at(std::uint64_t index) const {
    if (identity_view) return std::min(index, dataset->row_count() == 0 ? 0 : dataset->row_count() - 1);
    if (rows.empty()) return 0;
    return rows[static_cast<std::size_t>(std::min(index, rows.size() - 1))];
}

std::string_view App::cell(std::uint64_t row_index, std::uint32_t column_index) const {
    if (visible_count() == 0) return {};
    return dataset->cell(row_at(row_index), column_index);
}

GridLayout App::layout() const {
    GridLayout grid;
    grid.gutter = digit_count(visible_count()) + 4;
    grid.content_x = grid.gutter;
    grid.content_width = std::max(10, screen.width() - grid.gutter - 1);
    grid.header_y = 1;
    grid.first_row_y = 3;
    grid.status_y = std::max(4, screen.height() - 2);
    grid.hint_y = screen.height() - 1;
    grid.visible_rows = std::max(1, grid.status_y - grid.first_row_y);

    int x = grid.content_x;
    for (std::uint32_t c = first_column; c < dataset->column_count(); ++c) {
        int width = widths[c];
        // Never draw a column that would be clipped: a half-visible column
        // reads as a rendering bug, so leave the space empty and show the › flag.
        if (x + std::min(width, 8) > screen.width() - 1) break;
        width = std::min(width, screen.width() - x - 1);
        grid.columns.push_back(c);
        grid.x.push_back(x);
        grid.width.push_back(width);
        x += width + 1;
    }
    return grid;
}

void App::ensure_visible() {
    const std::uint64_t count = visible_count();
    if (count == 0) {
        cursor = 0;
        top_row = 0;
        return;
    }
    cursor = std::min(cursor, count - 1);
    const int visible = layout().visible_rows;
    if (cursor < top_row) top_row = cursor;
    if (visible > 0 && cursor >= top_row + static_cast<std::uint64_t>(visible)) {
        top_row = cursor - static_cast<std::uint64_t>(visible) + 1;
    }
    if (column < first_column) first_column = column;
    for (int guard = 0; guard < 4096; ++guard) {
        const GridLayout grid = layout();
        if (std::find(grid.columns.begin(), grid.columns.end(), column) != grid.columns.end()) break;
        if (first_column >= column) break;
        ++first_column;
    }
    if (first_column > column) first_column = column;
}

void App::move_cursor(std::int64_t delta) {
    const std::uint64_t count = visible_count();
    if (count == 0) return;
    const std::int64_t target = static_cast<std::int64_t>(cursor) + delta;
    cursor = static_cast<std::uint64_t>(std::clamp<std::int64_t>(target, 0, static_cast<std::int64_t>(count - 1)));
    ensure_visible();
    needs_redraw = true;
}

void App::move_column(std::int64_t delta) {
    if (dataset->column_count() == 0) return;
    const std::int64_t target = static_cast<std::int64_t>(column) + delta;
    column = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(target, 0, static_cast<std::int64_t>(dataset->column_count() - 1)));
    ensure_visible();
    needs_redraw = true;
}

void App::set_toast(std::string text, bool error) {
    toast.text = std::move(text);
    toast.error = error;
    toast.until = now_seconds() + 3.2;
}

// ── Jobs ────────────────────────────────────────────────────────────────────
void App::start_job(std::string phase, std::function<void(BackgroundJob&)> work,
                    std::function<void(App&, BackgroundJob&)> on_done) {
    cancel_job();
    job = std::make_unique<BackgroundJob>();
    BackgroundJob* raw = job.get();
    raw->phase = std::move(phase);
    raw->report = [raw](double fraction, std::string_view label) {
        raw->progress.store(fraction, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(raw->mutex);
        raw->phase.assign(label);
    };
    raw->apply = std::move(on_done);
    raw->worker = std::thread([raw, work = std::move(work)]() mutable {
        try {
            work(*raw);
        } catch (const Canceled&) {
            // Leave the results empty; the UI discards this job.
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->error = error.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(raw->mutex);
            raw->error = "unexpected failure";
        }
        raw->finished.store(true, std::memory_order_release);
    });
    needs_redraw = true;
}

void App::poll_job() {
    if (!job || !job->finished.load(std::memory_order_acquire)) return;
    if (job->worker.joinable()) job->worker.join();
    std::string error;
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        error = job->error;
    }
    if (!error.empty()) {
        set_toast(error, true);
    } else if (job->apply) {
        job->apply(*this, *job);
    }
    job.reset();
    needs_redraw = true;
}

void App::cancel_job() {
    if (!job) return;
    job->token.cancel();
    if (job->worker.joinable()) job->worker.join();
    job.reset();
    needs_redraw = true;
}

// ── View operations ─────────────────────────────────────────────────────────
void App::refresh_view() {
    const bool has_filter = filter_active && !filter_text.empty();
    const bool has_sort = sort_column.has_value();
    if (!has_filter && !has_sort) {
        cancel_job();
        identity_view = true;
        rows.clear();
        cursor = 0;
        top_row = 0;
        filter_label.clear();
        highlight.clear();
        sort_label.clear();
        needs_redraw = true;
        return;
    }

    auto ds = dataset;
    const std::string text = filter_text;
    const bool is_search = filter_is_search;
    const auto sort_col = sort_column;
    const bool descending = sort_descending;

    std::shared_ptr<query::Expr> predicate;
    if (has_filter && !is_search) {
        std::string error;
        predicate = query::parse_expression(text, *ds, error);
        if (!predicate) {
            set_toast("filter error: " + error, true);
            return;
        }
    }

    start_job("building view", [ds, predicate, text, has_filter, sort_col,
                                descending](BackgroundJob& bg) {
        query::RunOptions run;
        run.cancel = &bg.token;
        run.progress = bg.report;
        std::vector<std::uint64_t> result;
        if (has_filter) {
            result = predicate ? query::filter_rows(*ds, *predicate, run)
                               : query::search_rows(*ds, text, {}, run);
        } else {
            result = query::all_rows(*ds);
        }
        if (sort_col.has_value()) {
            query::OrderKey key;
            key.expr = query::Expr::column_ref(*sort_col, std::string(ds->column_name(*sort_col)));
            key.descending = descending;
            std::vector<query::OrderKey> keys;
            keys.push_back(std::move(key));
            query::sort_rows(*ds, result, keys, run);
        }
        bg.rows = std::move(result);
    }, [](App& app, BackgroundJob& bg) {
        app.rows = std::move(bg.rows);
        app.identity_view = false;
        const std::uint64_t count = app.rows.size();
        app.cursor = count == 0 ? 0 : std::min(app.cursor, count - 1);
        app.top_row = 0;
        app.ensure_visible();
        app.needs_redraw = true;
        if (count == 0) app.set_toast("no rows match", true);
    });
}

void App::apply_filter(std::string text, bool as_search) {
    const std::string trimmed(trim(text));
    mode = Mode::Browse;
    prompt_error.clear();
    if (trimmed.empty()) {
        clear_filter();
        return;
    }

    bool search = as_search;
    if (!search) {
        std::string error;
        auto predicate = query::parse_expression(trimmed, *dataset, error);
        if (!predicate) {
            // A bare word is almost always a search; punctuation means the user
            // meant an expression and deserves the parse error.
            if (trimmed.find_first_of("=<>!()'\"") != std::string::npos) {
                set_toast("filter: " + error.substr(0, 160), true);
                return;
            }
            search = true;
        }
    }

    filter_active = true;
    filter_text = trimmed;
    filter_is_search = search;
    filter_label = search ? ("search \"" + trimmed + "\"") : trimmed;
    highlight = search ? trimmed : "";
    cursor = 0;
    refresh_view();
}

void App::apply_sort(std::uint32_t col, bool descending) {
    if (col >= dataset->column_count()) return;
    sort_column = col;
    sort_descending = descending;
    sort_label = std::string(dataset->column_name(col)) + (descending ? " ↓" : " ↑");
    cursor = 0;
    refresh_view();
}

void App::clear_filter() {
    filter_active = false;
    filter_text.clear();
    filter_label.clear();
    highlight.clear();
    cursor = 0;
    refresh_view();
}

void App::clear_sort() {
    sort_column.reset();
    sort_label.clear();
    refresh_view();
}

void App::start_profile(std::uint32_t col) {
    if (col >= dataset->column_count()) return;
    profile_column = col;
    auto ds = dataset;
    std::vector<std::uint64_t> snapshot = identity_view ? std::vector<std::uint64_t>{} : rows;
    const bool all = identity_view;
    start_job("profiling", [ds, col, snapshot, all](BackgroundJob& bg) {
        ProfileOptions options;
        options.cancel = &bg.token;
        options.progress = bg.report;
        if (!all) options.rows = &snapshot;
        bg.profile = std::make_shared<ColumnProfile>(tess::profile_column(*ds, col, options));
    }, [](App& app, BackgroundJob& bg) {
        app.profile = std::move(*bg.profile);
        app.mode = Mode::Profile;
        app.profile_scroll = 0;
        app.needs_redraw = true;
    });
}

void App::start_export(std::string path) {
    if (trim(path).empty()) {
        set_toast("export needs a file name", true);
        return;
    }
    auto ds = dataset;
    std::vector<std::uint64_t> snapshot = identity_view ? std::vector<std::uint64_t>{} : rows;
    const bool all = identity_view;
    mode = Mode::Browse;
    start_job("exporting", [ds, snapshot, all, path](BackgroundJob& bg) {
        query::RunOptions run;
        run.cancel = &bg.token;
        run.progress = bg.report;
        const io::ExportStats stats = io::export_rows(*ds, all ? nullptr : &snapshot, {}, path,
                                                     io::OutputFormat::Csv, run);
        bg.exported_rows = stats.rows;
        bg.exported_bytes = stats.bytes;
        bg.elapsed = stats.seconds;
    }, [path](App& app, BackgroundJob& bg) {
        app.set_toast("exported " + format_count(bg.exported_rows) + " rows · " +
                      format_bytes(bg.exported_bytes) + " · " + format_double(bg.elapsed, 2) + " s → " + path);
    });
}

void App::reload() {
    const std::string path = dataset->info().path;
    if (path.empty() || !file_exists(path)) {
        set_toast("this data lives in memory and cannot be reloaded", true);
        return;
    }
    try {
        auto fresh = Dataset::open(path, OpenOptions{});
        dataset = fresh;
        compute_widths();
        filter_active = false;
        filter_text.clear();
        filter_label.clear();
        highlight.clear();
        sort_column.reset();
        sort_label.clear();
        identity_view = true;
        rows.clear();
        cursor = 0;
        top_row = 0;
        first_column = 0;
        column = 0;
        set_toast("reloaded " + format_count(dataset->row_count()) + " rows");
    } catch (const Error& error) {
        set_toast(error.what(), true);
    }
    needs_redraw = true;
}

// ── Input ───────────────────────────────────────────────────────────────────
void App::handle_key(const KeyEvent& event) {
    needs_redraw = true;

    if (event.key == Key::Resize) {
        screen.resize(terminal.width(), terminal.height());
        screen.invalidate();
        ensure_visible();
        return;
    }

    if (event.key == Key::MouseScrollUp) {
        move_cursor(-3);
        return;
    }
    if (event.key == Key::MouseScrollDown) {
        move_cursor(3);
        return;
    }
    if (event.key == Key::MouseClick) {
        const GridLayout grid = layout();
        if (event.mouse_y >= grid.first_row_y && event.mouse_y < grid.first_row_y + grid.visible_rows) {
            const std::uint64_t index = top_row + static_cast<std::uint64_t>(event.mouse_y - grid.first_row_y);
            if (index < visible_count()) {
                cursor = index;
                ensure_visible();
            }
        }
        return;
    }

    // Global escape hatches.
    if (event.key == Key::Ctrl && (event.ch == 'c' || event.ch == 'q')) {
        running = false;
        return;
    }

    // ── Prompt modes share text editing ─────────────────────────────────────
    if (mode == Mode::Filter || mode == Mode::Command || mode == Mode::Export) {
        switch (event.key) {
            case Key::Esc:
                mode = Mode::Browse;
                prompt_error.clear();
                return;
            case Key::Enter: {
                const std::string text = prompt;
                prompt_error.clear();
                if (mode == Mode::Filter) {
                    apply_filter(text, prompt_as_search);
                } else if (mode == Mode::Export) {
                    start_export(text);
                } else {
                    // Command palette.
                    const auto [command, argument] = split_command(text);
                    mode = Mode::Browse;
                    if (command.empty()) return;
                    if (command == "q" || command == "quit") {
                        running = false;
                    } else if (command == "sort") {
                        const auto index = dataset->column_index(argument);
                        if (!index.has_value()) set_toast("unknown column '" + argument + "'", true);
                        else apply_sort(*index, false);
                    } else if (command == "sortdesc" || command == "sort_desc") {
                        const auto index = dataset->column_index(argument);
                        if (!index.has_value()) set_toast("unknown column '" + argument + "'", true);
                        else apply_sort(*index, true);
                    } else if (command == "filter") {
                        apply_filter(argument, false);
                    } else if (command == "search") {
                        apply_filter(argument, true);
                    } else if (command == "clear") {
                        filter_active = false;
                        filter_text.clear();
                        sort_column.reset();
                        sort_label.clear();
                        cursor = 0;
                        refresh_view();
                        set_toast("view reset");
                    } else if (command == "export") {
                        start_export(argument.empty() ? default_export_name(*dataset) : argument);
                    } else if (command == "goto") {
                        std::int64_t target = 0;
                        if (parse_int(argument, target) && target > 0) {
                            cursor = static_cast<std::uint64_t>(target - 1);
                            ensure_visible();
                        } else {
                            set_toast("goto expects a row number", true);
                        }
                    } else if (command == "profile") {
                        if (argument.empty()) {
                            start_profile(column);
                        } else {
                            const auto index = dataset->column_index(argument);
                            if (!index.has_value()) set_toast("unknown column '" + argument + "'", true);
                            else start_profile(*index);
                        }
                    } else if (command == "reload") {
                        reload();
                    } else if (command == "help") {
                        mode = Mode::Help;
                    } else {
                        set_toast("unknown command '" + command + "'", true);
                    }
                }
                return;
            }
            case Key::Backspace: prompt_backspace(*this); return;
            case Key::Delete: prompt_delete(*this); return;
            case Key::Left: prompt_cursor = previous_rune(prompt, prompt_cursor); return;
            case Key::Right: prompt_cursor = next_rune(prompt, prompt_cursor); return;
            case Key::Home: prompt_cursor = 0; return;
            case Key::End: prompt_cursor = prompt.size(); return;
            case Key::Ctrl:
                if (event.ch == 'u') {
                    prompt.clear();
                    prompt_cursor = 0;
                } else if (event.ch == 'a') {
                    prompt_cursor = 0;
                } else if (event.ch == 'e') {
                    prompt_cursor = prompt.size();
                } else if (event.ch == 'w') {
                    while (prompt_cursor > 0 && prompt[prompt_cursor - 1] == ' ') prompt_backspace(*this);
                    while (prompt_cursor > 0 && prompt[prompt_cursor - 1] != ' ') prompt_backspace(*this);
                }
                return;
            case Key::Char: prompt_insert(*this, event.ch); return;
            default: return;
        }
    }

    // ── Overlays ────────────────────────────────────────────────────────────
    if (mode == Mode::Help) {
        if (event.key == Key::Esc || event.key == Key::Enter || event.is_alpha('q') || event.is('?')) {
            mode = Mode::Browse;
        }
        return;
    }

    if (mode == Mode::Sort) {
        const std::uint32_t count = dataset->column_count();
        if (count == 0) {
            mode = Mode::Browse;
            return;
        }
        switch (event.key) {
            case Key::Esc:
                mode = Mode::Browse;
                return;
            case Key::Up:
                menu_index = (menu_index + count - 1) % count;
                return;
            case Key::Down:
            case Key::Tab:
                menu_index = (menu_index + 1) % count;
                return;
            case Key::Home:
                menu_index = 0;
                return;
            case Key::End:
                menu_index = count - 1;
                return;
            case Key::Enter:
                apply_sort(menu_index, menu_descending);
                mode = Mode::Browse;
                menu_descending = false;
                return;
            case Key::Char:
                if (event.ch >= '0' && event.ch <= '9') {
                    const std::uint32_t digit = static_cast<std::uint32_t>(event.ch - '0');
                    if (digit >= 1 && digit <= count) {
                        apply_sort(digit - 1, menu_descending);
                        mode = Mode::Browse;
                        menu_descending = false;
                    }
                    return;
                }
                if (event.is_alpha('d')) {
                    apply_sort(menu_index, true);
                    mode = Mode::Browse;
                    menu_descending = false;
                    return;
                }
                if (event.is_alpha('a')) {
                    apply_sort(menu_index, false);
                    mode = Mode::Browse;
                    return;
                }
                if (event.is_alpha('c')) {
                    clear_sort();
                    mode = Mode::Browse;
                    return;
                }
                if (event.is_alpha('s')) {
                    menu_descending = !menu_descending;
                    return;
                }
                return;
            default: return;
        }
    }

    if (mode == Mode::Profile) {
        switch (event.key) {
            case Key::Esc:
            case Key::Enter:
                mode = Mode::Browse;
                return;
            case Key::Left:
                profile_scroll = 0;
                start_profile(column > 0 ? column - 1 : dataset->column_count() - 1);
                return;
            case Key::Right:
                profile_scroll = 0;
                start_profile((column + 1) % std::max<std::uint32_t>(1, dataset->column_count()));
                return;
            case Key::Up:
                profile_scroll = profile_scroll > 0 ? profile_scroll - 1 : 0;
                return;
            case Key::Down:
                ++profile_scroll;
                return;
            case Key::PageUp:
                profile_scroll = profile_scroll > 8 ? profile_scroll - 8 : 0;
                return;
            case Key::PageDown:
                profile_scroll += 8;
                return;
            case Key::Char:
                if (event.is_alpha('q')) mode = Mode::Browse;
                return;
            default:
                return;
        }
    }

    if (mode == Mode::Record) {
        switch (event.key) {
            case Key::Esc:
            case Key::Enter:
            case Key::Char:
                if (event.key != Key::Char || event.is_alpha('q') || event.is_alpha('i')) mode = Mode::Browse;
                return;
            case Key::Down:
                ++profile_scroll;
                return;
            case Key::Up:
                profile_scroll = profile_scroll > 0 ? profile_scroll - 1 : 0;
                return;
            case Key::PageDown:
                profile_scroll += 8;
                return;
            case Key::PageUp:
                profile_scroll = profile_scroll > 8 ? profile_scroll - 8 : 0;
                return;
            default:
                return;
        }
    }

    // ── Browse ──────────────────────────────────────────────────────────────
    switch (event.key) {
        case Key::Up:
            move_cursor(-1);
            return;
        case Key::Down:
            move_cursor(1);
            return;
        case Key::PageUp:
            move_cursor(-static_cast<std::int64_t>(layout().visible_rows));
            return;
        case Key::PageDown:
            move_cursor(static_cast<std::int64_t>(layout().visible_rows));
            return;
        case Key::Home:
            cursor = 0;
            ensure_visible();
            return;
        case Key::End:
            cursor = visible_count() == 0 ? 0 : visible_count() - 1;
            ensure_visible();
            return;
        case Key::Left:
            move_column(-1);
            return;
        case Key::Right:
            move_column(1);
            return;
        case Key::Enter:
            if (dataset->column_count() > 0) {
                mode = Mode::Record;
                profile_scroll = 0;
            }
            return;
        case Key::Esc:
            if (filter_active) clear_filter();
            return;
        case Key::Ctrl:
            if (event.ch == 'l') {
                screen.invalidate();
            } else if (event.ch == 'd') {
                move_cursor(static_cast<std::int64_t>(layout().visible_rows));
            } else if (event.ch == 'u') {
                move_cursor(-static_cast<std::int64_t>(layout().visible_rows));
            } else if (event.ch == 'n') {
                move_cursor(1);
            } else if (event.ch == 'p') {
                move_cursor(-1);
            }
            return;
        case Key::Char:
            break;
        default:
            return;
    }

    switch (event.ch) {
        case U'q':
            running = false;
            return;
        case U'j':
            move_cursor(1);
            return;
        case U'k':
            move_cursor(-1);
            return;
        case U'h':
            move_column(-1);
            return;
        case U'l':
            move_column(1);
            return;
        case U'g':
            cursor = 0;
            ensure_visible();
            return;
        case U'G':
            cursor = visible_count() == 0 ? 0 : visible_count() - 1;
            ensure_visible();
            return;
        case U'/':
            mode = Mode::Filter;
            prompt_as_search = true;
            prompt = filter_is_search ? filter_text : std::string();
            prompt_cursor = prompt.size();
            prompt_title = "search";
            prompt_error.clear();
            return;
        case U'f':
            mode = Mode::Filter;
            prompt_as_search = false;
            prompt = filter_active && !filter_is_search ? filter_text : std::string();
            prompt_cursor = prompt.size();
            prompt_title = "filter";
            prompt_error.clear();
            return;
        case U':':
            mode = Mode::Command;
            prompt.clear();
            prompt_cursor = 0;
            prompt_title = "command";
            prompt_error.clear();
            return;
        case U's':
            mode = Mode::Sort;
            menu_index = column;
            menu_descending = false;
            return;
        case U'p':
            start_profile(column);
            return;
        case U'e':
            mode = Mode::Export;
            prompt = default_export_name(*dataset);
            prompt_cursor = prompt.size();
            prompt_title = "export";
            prompt_error.clear();
            return;
        case U'y': {
            if (visible_count() == 0) return;
            const std::string value = dataset->decode_cell(row_at(cursor), column);
            terminal.copy_to_clipboard(value);
            set_toast("copied " + format_count(value.size()) + " bytes to the clipboard");
            return;
        }
        case U'r':
            reload();
            return;
        case U't':
            show_types = !show_types;
            set_toast(show_types ? "type row on" : "type row off");
            return;
        case U'?':
            mode = Mode::Help;
            return;
        default:
            return;
    }
}

// ── Status strings ──────────────────────────────────────────────────────────
std::string App::status_left() const {
    std::string out = " ";
    const std::uint64_t count = visible_count();
    if (count == 0) {
        out += "no rows";
    } else {
        const std::uint64_t last = std::min<std::uint64_t>(top_row + static_cast<std::uint64_t>(layout().visible_rows), count);
        out += "rows " + format_count(top_row + 1) + "–" + format_count(last) + " of " + format_count(count);
        out += "  ·  cursor " + format_count(cursor + 1);
    }
    out += "  ·  " + std::string(dataset->column_name(column)) + "  " +
           std::string(to_string(dataset->column_type(column)));
    return out;
}

std::string App::status_right() const {
    std::string out;
    if (filter_active) out += "filtered  ";
    if (sort_column.has_value()) out += "sorted by " + sort_label + "  ";
    out += std::string(to_string(dataset->info().format));
    out += "  ·  " + std::string(to_string(dataset->info().encoding));
    out += "  ·  " + format_bytes(dataset->info().bytes) + " ";
    return out;
}

std::vector<RecordLine> App::record_lines(int width) const {
    std::vector<RecordLine> lines;
    if (visible_count() == 0) return lines;
    const std::uint64_t row = row_at(cursor);
    for (std::uint32_t c = 0; c < dataset->column_count(); ++c) {
        const ColumnMeta& meta = dataset->columns()[c];
        lines.push_back({true, std::string(meta.name), std::string(to_string(meta.type))});
        const std::string value = dataset->decode_cell(row, c);
        if (value.empty()) {
            lines.push_back({false, "(empty)", {}});
        } else {
            const int usable = std::max(10, width - 4);
            std::size_t index = 0;
            std::string line = "  ";
            int used = 0;
            while (index < value.size()) {
                const Rune rune = decode_rune(value, index);
                const int rune_columns = std::max(1, rune_width(rune.codepoint));
                if (used + rune_columns > usable) {
                    lines.push_back({false, line, {}});
                    line = "  ";
                    used = 0;
                }
                if (rune.codepoint < 32 || (rune.codepoint >= 0x7F && rune.codepoint < 0xA0)) {
                    line.append("·");
                    ++used;
                } else {
                    line.append(value.substr(index, static_cast<std::size_t>(rune.length)));
                    used += rune_columns;
                }
                index += static_cast<std::size_t>(rune.length);
            }
            lines.push_back({false, line, {}});
        }
        lines.push_back({false, std::string(), {}});
    }
    return lines;
}

// ── Event loop ──────────────────────────────────────────────────────────────
int App::run() {
    terminal.init();
    terminal.set_title(std::string("tessera — ") + dataset->info().name);
    screen.resize(terminal.width(), terminal.height());
    screen.invalidate();
    compute_widths();
    ensure_visible();

    while (running) {
        poll_job();
        const double now = now_seconds();
        if (needs_redraw || busy() || toast.visible(now)) {
            render_frame(*this);
            needs_redraw = false;
            ++frames;
        }
        KeyEvent event;
        const int timeout = (busy() || toast.visible(now)) ? 40 : 120;
        if (terminal.read_key(event, timeout)) {
            if (busy() && event.key == Key::Esc) {
                cancel_job();
                set_toast("cancelled");
            } else {
                handle_key(event);
            }
        } else if (busy()) {
            needs_redraw = true;
        }
    }

    cancel_job();
    terminal.shutdown();
    return 0;
}

int run(std::shared_ptr<Dataset> dataset) {
    App app(std::move(dataset));
    return app.run();
}

} // namespace tess::tui
