// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · app.hpp
//  Explorer state and the event loop. Rendering lives in views.cpp; everything
//  that mutates state or schedules background work lives here.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/dataset.hpp"
#include "core/profile.hpp"
#include "query/exec.hpp"
#include "tui/screen.hpp"
#include "tui/terminal.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tess::tui {

enum class Mode { Browse, Filter, Sort, Command, Help, Profile, Record, Export };

struct App;

struct Toast {
    std::string text;
    bool error = false;
    double until = 0.0;
    [[nodiscard]] bool visible(double now) const { return !text.empty() && now < until; }
};

struct BackgroundJob {
    std::thread worker;
    CancelToken token;
    std::atomic<double> progress{0.0};
    std::atomic<bool> finished{false};
    std::mutex mutex;
    std::string phase = "working";
    std::string error;

    std::vector<std::uint64_t> rows;
    std::shared_ptr<ColumnProfile> profile;
    ProgressFn report;
    std::uint64_t exported_rows = 0;
    std::uint64_t exported_bytes = 0;
    double elapsed = 0.0;

    std::function<void(App&, BackgroundJob&)> apply;
};

struct RecordLine {
    bool label = false;
    std::string text;
    std::string meta; ///< column type, shown right-aligned on label lines
};

struct GridLayout {
    int gutter = 5;
    int content_x = 0;
    int content_width = 0;
    int header_y = 1;
    int first_row_y = 3;
    int visible_rows = 1;
    int status_y = 0;
    int hint_y = 0;
    std::vector<std::uint32_t> columns;
    std::vector<int> x;
    std::vector<int> width;
};

struct App {
    explicit App(std::shared_ptr<Dataset> dataset);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int run();

    // ── State ────────────────────────────────────────────────────────────────
    std::shared_ptr<Dataset> dataset;
    Terminal terminal;
    Screen screen{100, 30};

    std::vector<std::uint64_t> rows; ///< empty + identity_view when unfiltered
    bool identity_view = true;
    std::vector<int> widths;

    std::string filter_text;   ///< the expression or search term (as typed)
    std::string filter_label;  ///< human-readable active filter
    std::string highlight;     ///< substring to highlight in cells
    bool filter_active = false;
    bool show_types = false;
    bool filter_is_search = false;
    bool prompt_as_search = false; ///< '/' searches, 'f' parses an expression
    std::optional<std::uint32_t> sort_column;
    bool sort_descending = false;
    std::string sort_label;

    std::uint64_t cursor = 0;       ///< index into the visible rows
    std::uint64_t top_row = 0;      ///< first visible index
    std::uint32_t column = 0;       ///< absolute column index
    std::uint32_t first_column = 0; ///< leftmost visible column

    Mode mode = Mode::Browse;
    std::string prompt;
    std::size_t prompt_cursor = 0;
    std::string prompt_error;
    std::string prompt_title;

    std::uint32_t menu_index = 0;
    bool menu_descending = false;

    std::optional<ColumnProfile> profile;
    std::uint32_t profile_column = 0;
    std::uint64_t profile_scroll = 0;

    std::unique_ptr<BackgroundJob> job;
    Toast toast;

    bool running = true;
    bool needs_redraw = true;
    double started_at = 0.0;
    std::uint64_t frames = 0;

    // ── Operations ───────────────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t visible_count() const;
    [[nodiscard]] std::uint64_t row_at(std::uint64_t index) const;
    [[nodiscard]] std::string_view cell(std::uint64_t row_index, std::uint32_t column) const;
    [[nodiscard]] GridLayout layout() const;

    void handle_key(const KeyEvent& event);
    void apply_filter(std::string text, bool as_search);
    void apply_sort(std::uint32_t column, bool descending);
    void refresh_view();
    void clear_filter();
    void clear_sort();
    void start_profile(std::uint32_t column);
    void start_export(std::string path);
    void reload();
    void move_cursor(std::int64_t delta);
    void move_column(std::int64_t delta);
    void ensure_visible();
    void set_toast(std::string text, bool error = false);
    void start_job(std::string phase, std::function<void(BackgroundJob&)> work,
                   std::function<void(App&, BackgroundJob&)> on_done);
    void poll_job();
    void cancel_job();
    [[nodiscard]] bool busy() const { return job != nullptr; }

    // ── Helpers used by the renderer ─────────────────────────────────────────
    void compute_widths();
    [[nodiscard]] std::string status_left() const;
    [[nodiscard]] std::string status_right() const;
    [[nodiscard]] std::vector<RecordLine> record_lines(int width) const;
};

/// Renders the whole frame (grid + overlay + bars) and flushes it.
void render_frame(App& app);

int run(std::shared_ptr<Dataset> dataset);

} // namespace tess::tui
