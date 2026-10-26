// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · views.cpp
//  All drawing for the explorer. Nothing here mutates application state, which
//  makes the whole UI reviewable by reading one file top to bottom.
// ─────────────────────────────────────────────────────────────────────────────
#include "tui/app.hpp"

#include "common/util.hpp"
#include "io/format.hpp"
#include "tui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace tess::tui {
namespace {

using namespace tess::tui::theme;

Style type_style(ColumnType type, bool on_selection) {
    const int bg = on_selection ? kSelectionBg : kPageBg;
    switch (type) {
        case ColumnType::Integer:
        case ColumnType::Float: return Style::fg_bg(kNumeric, bg);
        case ColumnType::Timestamp: return Style::fg_bg(kTemporal, bg);
        case ColumnType::Boolean: return Style::fg_bg(kBoolean, bg);
        case ColumnType::Unknown: return Style::fg_bg(kNull, bg);
        default: return Style::fg_bg(kText, bg);
    }
}

[[maybe_unused]] Style with_bg(Style style, int bg) {
    style.bg = static_cast<std::int16_t>(bg);
    return style;
}

void fill_row(Screen& screen, int y, const Style& style) {
    for (int x = 0; x < screen.width(); ++x) screen.put(x, y, " ", style);
}

void draw_bar(Screen& screen, int y, const Style& base) {
    fill_row(screen, y, base);
}

void draw_right(Screen& screen, int y, int right_edge, std::string_view text, const Style& style) {
    const int width = text_width(text);
    const int x = right_edge - width;
    if (x < 0) {
        screen.put(0, y, truncate_width(text, right_edge), style);
        return;
    }
    screen.put(x, y, text, style);
}

std::string zero_padded(std::uint64_t value, int width) {
    std::string text = format_count(value);
    if (static_cast<int>(text.size()) < width) text.insert(0, static_cast<std::size_t>(width) - text.size(), ' ');
    return text;
}

/// Draws text clipped to `width`, painting case-insensitive hits of `needle`.
void draw_text_with_hits(Screen& screen, int x, int y, std::string_view raw, int width, const Style& base,
                         const Style& hit, bool right_align, std::string_view needle) {
    if (width <= 0) return;
    std::string text = truncate_width(raw, width);
    const int pad = std::max(0, width - text_width(text));
    if (right_align) x += pad;

    if (needle.empty() || text.empty()) {
        screen.put(x, y, text, base);
        return;
    }

    const std::string lowered = to_lower(text);
    const std::string lowered_needle = to_lower(needle);
    std::size_t position = 0;
    int cursor = x;
    while (position < text.size()) {
        const std::size_t found = lowered.find(lowered_needle, position);
        if (found == std::string::npos) {
            screen.put(cursor, y, std::string_view(text).substr(position), base);
            break;
        }
        if (found > position) {
            screen.put(cursor, y, std::string_view(text).substr(position, found - position), base);
            cursor += text_width(std::string_view(text).substr(position, found - position));
        }
        const std::size_t length = std::min(lowered_needle.size(), text.size() - found);
        screen.put(cursor, y, std::string_view(text).substr(found, length), hit);
        cursor += text_width(std::string_view(text).substr(found, length));
        position = found + std::max<std::size_t>(1, length);
    }
}

int panel_w_guess(const Screen& screen);

struct Panel {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] int inner_x() const { return x + 2; }
    [[nodiscard]] int inner_y() const { return y + 2; }
    [[nodiscard]] int inner_w() const { return w - 4; }
    [[nodiscard]] int inner_h() const { return h - 4; }
};

Panel make_panel(const Screen& screen, int width, int height) {
    Panel panel;
    panel.w = std::clamp(width, 24, std::max(24, screen.width() - 2));
    panel.h = std::clamp(height, 5, std::max(5, screen.height() - 2));
    panel.x = (screen.width() - panel.w) / 2;
    panel.y = (screen.height() - panel.h) / 2;
    return panel;
}

/// Solid surface with a titled top border and a hint embedded in the bottom one.
void panel_frame(Screen& screen, const Panel& panel, std::string_view title, std::string_view meta,
                 std::string_view hint) {
    for (int y = panel.y; y < panel.y + panel.h; ++y) {
        for (int x = panel.x; x < panel.x + panel.w; ++x) {
            screen.put(x, y, " ", Style::fg_bg(kText, kPanelBg));
        }
    }
    screen.box(panel.x, panel.y, panel.w, panel.h, Style::fg_bg(kBorder, kPanelBg), true);
    if (!title.empty()) {
        const std::string label = " " + std::string(title) + " ";
        screen.put(panel.x + 2, panel.y, label, Style::fg_bg(kAccent, kPanelBg).bold());
    }
    if (!meta.empty()) {
        const std::string label = " " + std::string(meta) + " ";
        const int x = panel.x + panel.w - 3 - static_cast<int>(text_width(label));
        screen.put(std::max(panel.x + 2, x), panel.y, label, Style::fg_bg(kMuted, kPanelBg));
    }
    if (!hint.empty()) {
        const std::string label = " " + std::string(hint) + " ";
        const int x = panel.x + panel.w - 3 - static_cast<int>(text_width(label));
        screen.put(std::max(panel.x + 2, x), panel.y + panel.h - 1, label, Style::fg_bg(kFaint, kPanelBg));
    }
}

void panel_text(Screen& screen, const Panel& panel, int row, std::string_view text, const Style& style) {
    if (row < 0 || row >= panel.inner_h() + 1) return;
    screen.put(panel.inner_x(), panel.inner_y() + row,
               truncate_width(text, panel.inner_w()), style);
}

void panel_rule(Screen& screen, const Panel& panel, int row) {
    if (row < 0 || row >= panel.inner_h() + 1) return;
    for (int x = panel.x + 2; x < panel.x + panel.w - 2; ++x) {
        screen.put(x, panel.inner_y() + row, "─", Style::fg_bg(kMuted, kPanelBg));
    }
}

// ── Overlays ────────────────────────────────────────────────────────────────
void draw_sort_menu(App& app) {
    Screen& screen = app.screen;
    const std::uint32_t columns = app.dataset->column_count();
    const int rows_visible = std::max(1, std::min<int>(static_cast<int>(columns), screen.height() - 9));
    const Panel panel = make_panel(screen, 64, rows_visible + 5);
    panel_frame(screen, panel, "sort", format_count(columns) + " columns",
                "enter ascend · d descend · c clear · esc");

    int first = 0;
    if (app.menu_index >= static_cast<std::uint32_t>(rows_visible)) {
        first = static_cast<int>(app.menu_index) - rows_visible + 1;
    }
    for (int i = 0; i < rows_visible; ++i) {
        const int index = first + i;
        if (index >= static_cast<int>(columns)) break;
        const ColumnMeta& meta = app.dataset->columns()[static_cast<std::size_t>(index)];
        const int y = panel.inner_y() + i;
        const bool selected = static_cast<std::uint32_t>(index) == app.menu_index;
        const int background = selected ? kSelectionBg : kPanelBg;
        for (int x = panel.x + 1; x < panel.x + panel.w - 1; ++x) screen.put(x, y, " ", Style::fg_bg(kText, background));
        screen.put(panel.inner_x(), y, zero_padded(static_cast<std::uint64_t>(index + 1), 2),
                   Style::fg_bg(kFaint, background));
        screen.put(panel.inner_x() + 3, y, std::string(1, type_glyph(meta.type)),
                   Style::fg_bg(selected ? kAccent : kFaint, background));
        screen.put(panel.inner_x() + 5, y,
                   truncate_width(meta.name, panel.inner_w() - 24),
                   Style::fg_bg(selected ? kAccent : kText, background).bold());
        const std::string type_text(to_string(meta.type));
        screen.put(panel.x + panel.w - 3 - static_cast<int>(type_text.size()), y, type_text,
                   Style::fg_bg(kMuted, background));
        if (app.sort_column.has_value() && *app.sort_column == static_cast<std::uint32_t>(index)) {
            screen.put(panel.x + panel.w - 5 - static_cast<int>(type_text.size()), y,
                       app.sort_descending ? "↓" : "↑", Style::fg_bg(kAccent, background).bold());
        }
    }
}

void draw_help(App& app) {
    Screen& screen = app.screen;
    struct Row {
        const char* keys;
        const char* what;
    };
    static constexpr Row kRows[] = {
        {"↑ ↓ / j k", "move one row"},
        {"← → / h l", "move one column"},
        {"PgUp PgDn", "move one screen"},
        {"g / G", "first / last row"},
        {"⏎", "inspect the record"},
        {"/", "search all columns"},
        {"f", "filter by expression"},
        {"s", "sort by column"},
        {"p", "column profile"},
        {"y", "copy the cell"},
        {"t", "toggle type row"},
        {"e", "export this view"},
        {"r", "reload from disk"},
        {":", "command palette"},
        {"?", "this screen"},
        {"esc", "clear filter"},
        {"q", "quit"},
    };
    constexpr int kRowCount = static_cast<int>(std::size(kRows));
    constexpr int kColumnRows = (kRowCount + 1) / 2;
    const int height = kColumnRows + 11;
    const Panel panel = make_panel(screen, 86, height);
    panel_frame(screen, panel, "keys & syntax", std::string(TESSERA_VERSION), "any key closes");

    const int column_width = (panel.inner_w() - 4) / 2;
    for (int i = 0; i < kRowCount; ++i) {
        const int column = i / kColumnRows;
        const int row = i % kColumnRows;
        const int x = panel.inner_x() + column * (column_width + 4);
        const int y = panel.inner_y() + row;
        screen.put(x, y, kRows[i].keys, Style::fg_bg(kAccent, kPanelBg).bold());
        screen.put(x + 14, y, truncate_width(kRows[i].what, column_width - 14),
                   Style::fg_bg(kText, kPanelBg));
    }

    const int rule_y = panel.inner_y() + kColumnRows + 1;
    panel_rule(screen, panel, kColumnRows + 1);
    screen.put(panel.inner_x(), rule_y + 1, "filter syntax", Style::fg_bg(kMuted, kPanelBg).bold());
    screen.put(panel.inner_x() + 15, rule_y + 1,
               truncate_width("region = 'EU' and revenue > 1000", panel.inner_w() - 15),
               Style::fg_bg(kText, kPanelBg));
    screen.put(panel.inner_x(), rule_y + 2, "also", Style::fg_bg(kMuted, kPanelBg).bold());
    screen.put(panel.inner_x() + 15, rule_y + 2,
               truncate_width("name like 'a%'  ·  units between 2 and 9", panel.inner_w() - 15),
               Style::fg_bg(kText, kPanelBg));
    screen.put(panel.inner_x() + 15, rule_y + 3,
               truncate_width("date >= '2024-01-01'  ·  id in (3,4)  ·  region is not null",
                              panel.inner_w() - 15),
               Style::fg_bg(kText, kPanelBg));
    screen.put(panel.inner_x(), rule_y + 4,
               truncate_width("plain text falls back to a case-insensitive substring search", panel.inner_w()),
               Style::fg_bg(kFaint, kPanelBg));
}

void draw_profile(App& app) {
    Screen& screen = app.screen;
    if (!app.profile.has_value()) {
        const Panel panel = make_panel(screen, 60, 6);
        panel_frame(screen, panel, "profile", "", "esc");
        panel_text(screen, panel, 0, "no profile available for this column", Style::fg_bg(kMuted, kPanelBg));
        return;
    }
    const ColumnProfile& p = *app.profile;
    const int total_columns = static_cast<int>(app.dataset->column_count());

    std::vector<std::pair<std::string, Style>> lines;
    const Style normal = Style::fg_bg(kText, kPanelBg);
    const Style muted = Style::fg_bg(kMuted, kPanelBg);
    const Style accent = Style::fg_bg(kAccent, kPanelBg);

    std::string meta = "column " + std::to_string(app.profile_column + 1) + " of " + std::to_string(total_columns);
    std::string type_text(to_string(p.type));

    const double fill = p.total > 0 ? 1.0 - static_cast<double>(p.nulls) / static_cast<double>(p.total) : 0.0;
    std::string summary = "fill " + format_percent(fill, 1) + "   distinct " + format_count(p.distinct_estimate);
    if (p.distinct_is_estimate) summary += "~";
    summary += "   nulls " + format_count(p.nulls);
    lines.emplace_back(summary, muted);

    if (p.numeric) {
        lines.emplace_back("min " + format_double(p.min, 8) + "    p25 " + format_double(p.p25, 8) +
                               "    median " + format_double(p.median, 8) + "    p75 " + format_double(p.p75, 8) +
                               "    max " + format_double(p.max, 8),
                           normal);
        lines.emplace_back("mean " + format_double(p.mean, 8) + "    sd " + format_double(p.stddev, 8) +
                               "    p99 " + format_double(p.p99, 8),
                           normal);
    } else if (p.temporal) {
        lines.emplace_back("from  " + format_timestamp(p.time_min), normal);
        lines.emplace_back("to    " + format_timestamp(p.time_max), normal);
    } else {
        lines.emplace_back("length " + format_count(p.min_length) + "–" + format_count(p.max_length) +
                               "   mean " + format_double(p.mean_length, 2),
                           normal);
    }

    if (!p.histogram.empty()) {
        std::uint64_t peak = 0;
        for (const auto& bucket : p.histogram) peak = std::max(peak, bucket.count);
        if (peak > 0) {
            lines.emplace_back("", normal);
            lines.emplace_back("distribution", muted);
            const std::size_t width = static_cast<std::size_t>(std::max(20, std::min(panel_w_guess(screen), 64)));
            lines.emplace_back(io::sparkline(p.histogram, width), accent);
        }
    }

    // A column where almost every value is unique has no useful "top" list.
    const bool show_top = !p.top_values.empty() && (p.total == 0 || p.distinct_estimate * 2 < p.total) &&
                          p.type != ColumnType::Integer && p.type != ColumnType::Timestamp;
    if (!show_top && !p.numeric && !p.temporal && !p.top_values.empty()) {
        // Nearly-unique text: a frequency table would be meaningless, but the
        // actual values are the most useful thing we can show.
        lines.emplace_back("", normal);
        lines.emplace_back("examples", muted);
        for (std::size_t i = 0; i < p.top_values.size() && i < 5; ++i) {
            lines.emplace_back("  " + truncate_width(p.top_values[i].first, 54), normal);
        }
    }
    if (show_top) {
        lines.emplace_back("", normal);
        lines.emplace_back("most frequent", muted);
        for (std::size_t i = 0; i < p.top_values.size() && i < 4; ++i) {
            const auto& [value, count] = p.top_values[i];
            const double share = p.total > 0 ? static_cast<double>(count) / static_cast<double>(p.total) : 0.0;
            lines.emplace_back(truncate_width(value, 30) + "  " + format_count(count) + "  " +
                                   format_percent(share, 1),
                               normal);
        }
    }

    const int height = static_cast<int>(lines.size()) + 6;
    const Panel panel = make_panel(screen, std::min(screen.width() - 4, 78), height);
    panel_frame(screen, panel, "profile", meta, "←→ column · esc");

    screen.put(panel.inner_x(), panel.inner_y(), truncate_width(p.name, panel.inner_w() - 12), accent.bold());
    screen.put(panel.x + panel.w - 3 - static_cast<int>(type_text.size()), panel.inner_y(), type_text,
               Style::fg_bg(kMuted, kPanelBg));
    panel_rule(screen, panel, 1);

    int y = panel.inner_y() + 2;
    const int max_y = panel.y + panel.h - 2;
    for (std::size_t i = app.profile_scroll; i < lines.size() && y < max_y; ++i, ++y) {
        if (lines[i].first.empty()) continue;
        screen.put(panel.inner_x(), y, truncate_width(lines[i].first, panel.inner_w()), lines[i].second);
    }
}

int panel_w_guess(const Screen& screen) { return std::min(screen.width() - 4, 78); }

void draw_record(App& app) {
    Screen& screen = app.screen;
    // Size the inspector to its own content: a fixed-width panel leaves a dead
    // gutter on the right for short schemas and wraps needlessly for long ones.
    const auto probe = app.record_lines(std::max(20, screen.width() - 8));
    int widest = 0;
    for (const RecordLine& line : probe) {
        widest = std::max(widest, text_width(line.text) + static_cast<int>(line.meta.size()) + 6);
    }
    const int desired = std::clamp(widest, 46, std::min(96, screen.width() - 4));
    const auto lines = app.record_lines(desired - 4);
    const int visible = std::min<int>(static_cast<int>(lines.size()), std::max(3, screen.height() - 8));
    const Panel panel = make_panel(screen, desired, visible + 5);
    const std::string title = "row " + format_count(app.visible_count() == 0 ? 0 : app.row_at(app.cursor) + 1);
    const std::string first = format_count(app.profile_scroll + 1);
    const std::string last = format_count(std::min<std::size_t>(
        app.profile_scroll + static_cast<std::size_t>(visible), lines.size()));
    const std::string meta = first + "–" + last + " of " + format_count(lines.size());
    panel_frame(screen, panel, title, meta, "↑↓ scroll · esc");

    int y = panel.inner_y();
    for (std::size_t i = app.profile_scroll; i < lines.size() && y < panel.y + panel.h - 2; ++i, ++y) {
        const RecordLine& line = lines[i];
        if (line.label) {
            screen.put(panel.inner_x(), y, truncate_width(line.text, panel.inner_w() - 12),
                       Style::fg_bg(kAccent, kPanelBg).bold());
            if (!line.meta.empty()) {
                screen.put(panel.inner_x() + panel.inner_w() - static_cast<int>(line.meta.size()), y, line.meta,
                           Style::fg_bg(kFaint, kPanelBg));
            }
        } else if (line.text == "(empty)") {
            screen.put(panel.inner_x(), y, line.text, Style::fg_bg(kNull, kPanelBg));
        } else {
            screen.put(panel.inner_x(), y, truncate_width(line.text, panel.inner_w()),
                       Style::fg_bg(kStrong, kPanelBg));
        }
    }
}

void draw_progress(App& app) {
    Screen& screen = app.screen;
    const Panel panel = make_panel(screen, std::min(screen.width() - 4, 56), 7);
    panel_frame(screen, panel, "working", "", "esc cancels");
    std::string phase;
    {
        std::lock_guard<std::mutex> lock(app.job->mutex);
        phase = app.job->phase;
    }
    const double fraction = std::clamp(app.job->progress.load(), 0.0, 1.0);
    screen.put(panel.inner_x(), panel.inner_y(), truncate_width(phase, panel.inner_w()), Style::fg_bg(kText, kPanelBg));
    const std::string percent = format_percent(fraction, 0);
    screen.put(panel.x + panel.w - 3 - static_cast<int>(percent.size()), panel.inner_y() + 2, percent,
               Style::fg_bg(kMuted, kPanelBg));
    const int bar_width = panel.inner_w() - static_cast<int>(percent.size()) - 3;
    const int filled = static_cast<int>(fraction * static_cast<double>(bar_width));
    for (int i = 0; i < bar_width; ++i) {
        screen.put(panel.inner_x() + i, panel.inner_y() + 2, i < filled ? "█" : "░",
                   Style::fg_bg(i < filled ? kAccent : kBorder, kPanelBg));
    }
}


// ── Frame chrome ─────────────────────────────────────────────────────────────
void draw_brand_bar(App& app) {
    Screen& screen = app.screen;
    const int width = screen.width();
    draw_bar(screen, 0, Style::fg_bg(kText, kBarBg));

    int x = 2;
    screen.put(x, 0, "▚", Style::fg_bg(kAccent, kBarBg).bold());
    x += 2;
    screen.put(x, 0, "TESSERA", Style::fg_bg(kStrong, kBarBg).bold());
    x += 8;
    screen.put(x, 0, "▏", Style::fg_bg(kBorder, kBarBg));
    x += 2;

    std::string meta = format_bytes(app.dataset->source().size()) + "   " +
                       format_count(app.dataset->row_count()) + " rows × " +
                       format_count(app.dataset->column_count()) + " cols";
    if (app.dataset->info().open_seconds > 0.0) {
        meta += "   " + format_double(app.dataset->info().open_seconds * 1000.0, 3) + " ms";
    }
    const int meta_x = width - 2 - text_width(meta);
    if (meta_x > x + 8) {
        draw_right(screen, 0, width - 2, meta, Style::fg_bg(kFaint, kBarBg));
    }
    const int path_room = std::max(8, std::min(meta_x, width - 2) - x - 2);
    const std::string path = ellipsize_path(app.dataset->source().path(), path_room);
    screen.put(x, 0, truncate_width(path, path_room), Style::fg_bg(kMuted, kBarBg));
}

void draw_column_header(App& app, const GridLayout& grid) {
    Screen& screen = app.screen;
    const int width = screen.width();
    draw_bar(screen, grid.header_y, Style::fg_bg(kMuted, kPageBg));

    screen.put(std::max(0, grid.gutter - 6), grid.header_y, "row", Style::fg_bg(kFaint, kPageBg));
    screen.put(grid.gutter - 2, grid.header_y, "│", Style::fg_bg(kBorder, kPageBg));

    for (std::size_t i = 0; i < grid.columns.size(); ++i) {
        const std::uint32_t column = grid.columns[i];
        const ColumnMeta& meta = app.dataset->columns()[column];
        const bool focused = column == app.column;
        const bool sorted = app.sort_column.has_value() && *app.sort_column == column;
        const int cell_bg = focused ? kSelectionBg : kPageBg;

        // Repaint the whole header cell first: with type tags on, a name and a
        // type share the row and stale glyphs from a previous frame would show.
        for (int fill = 0; fill < grid.width[i]; ++fill) {
            screen.put(grid.x[i] + fill, grid.header_y, " ", Style::fg_bg(kText, cell_bg));
        }
        const Style name_style =
            focused ? Style::fg_bg(kAccent, cell_bg).bold() : Style::fg_bg(kMuted, cell_bg);
        std::string name = sanitize(meta.name);
        std::string kind = app.show_types ? std::string(to_string(meta.type)) : std::string();
        int name_room = grid.width[i];
        if (!kind.empty()) name_room = std::max(1, grid.width[i] - static_cast<int>(kind.size()) - 2);
        const std::string shown = truncate_width(name, name_room);
        screen.put(grid.x[i], grid.header_y, shown, name_style);
        int arrow_x = grid.x[i] + text_width(shown);
        if (sorted) {
            if (arrow_x + 1 < grid.x[i] + grid.width[i] && arrow_x < grid.x[i] + name_room) {
                screen.put(arrow_x, grid.header_y, app.sort_descending ? "↓" : "↑",
                           Style::fg_bg(kAccent, cell_bg).bold());
            }
        }
        if (!kind.empty() && grid.width[i] > static_cast<int>(kind.size()) + 2) {
            screen.put(grid.x[i] + grid.width[i] - static_cast<int>(kind.size()), grid.header_y, kind,
                       Style::fg_bg(kFaint, cell_bg));
        }
    }

    // Hairline rule beneath the header, with joints between columns.
    for (int x = std::max(0, grid.gutter - 2); x < width; ++x) {
        screen.put(x, grid.header_y + 1, "─", Style::fg_bg(kBorder, kPageBg));
    }
    for (std::size_t i = 1; i < grid.x.size(); ++i) {
        screen.put(grid.x[i] - 1, grid.header_y + 1, "┬", Style::fg_bg(kFaint, kPageBg));
    }

    // Scroll affordances when columns extend past either edge.
    const std::uint32_t last_shown = grid.columns.empty() ? 0 : grid.columns.back();
    if (last_shown + 1 < app.dataset->column_count()) {
        screen.put(width - 1, grid.header_y, "›", Style::fg_bg(kAccent, kPageBg).bold());
    }
    if (app.first_column > 0) {
        screen.put(std::max(0, grid.gutter - 1), grid.header_y, "‹", Style::fg_bg(kAccent, kPageBg).bold());
    }
}

void draw_grid(App& app, const GridLayout& grid) {
    Screen& screen = app.screen;
    const std::uint64_t count = app.visible_count();
    const Style bar_style = Style::fg_bg(kMuted, kBarBg);
    (void)bar_style;

    for (int row_y = 0; row_y < grid.visible_rows; ++row_y) {
        const int y = grid.first_row_y + row_y;
        if (y >= grid.status_y) break;
        const std::uint64_t index = app.top_row + static_cast<std::uint64_t>(row_y);
        if (index >= count) {
            for (int x = 0; x < screen.width(); ++x) screen.put(x, y, " ", Style::fg_bg(kText, kPageBg));
            continue;
        }
        const bool selected = index == app.cursor;
        const int bg = selected ? kSelectionBg : kPageBg;
        fill_row(screen, y, Style::fg_bg(kText, bg));

        // Row number gutter.
        const std::uint64_t number = app.row_at(index) + 1;
        std::string gutter_text = zero_padded(number, grid.gutter - 3);
        screen.put(0, y, gutter_text, selected ? Style::fg_bg(kAccent, bg).bold() : Style::fg_bg(kFaint, bg));
        screen.put(grid.gutter - 2, y, "│", Style::fg_bg(kBorder, bg));

        for (std::size_t i = 0; i < grid.columns.size(); ++i) {
            const std::uint32_t column = grid.columns[i];
            const ColumnMeta& meta = app.dataset->columns()[column];
            const std::string_view raw = app.cell(index, column);
            const bool numeric = meta.type == ColumnType::Integer || meta.type == ColumnType::Float;
            const bool is_cursor = selected && column == app.column;

            Style base = type_style(meta.type, selected);
            Style hit = Style::fg_bg(kSearchHitFg, kSearchHitBg);
            if (raw.empty()) {
                screen.put(grid.x[i], y, truncate_width("·", grid.width[i]), Style::fg_bg(kNull, bg));
                continue;
            }
            if (is_cursor) {
                base = Style::fg_bg(kCursorFg, kCursorBg).bold();
                hit = Style::fg_bg(kPageBg, kAccent);
            }
            draw_text_with_hits(screen, grid.x[i], y, raw, grid.width[i], base, hit, numeric,
                                app.filter_is_search ? std::string_view(app.highlight) : std::string_view());
        }
    }
}

void draw_status(App& app, const GridLayout& grid) {
    Screen& screen = app.screen;
    const int y = grid.status_y;
    draw_bar(screen, y, Style::fg_bg(kText, kBarBg));

    const std::uint64_t count = app.visible_count();
    const std::uint64_t shown = count == 0 ? 0 : std::min(app.cursor + 1, count);
    std::string right = "row " + format_count(shown) + " of " + format_count(count);
    if (count > 0) {
        right += "   " + format_percent(static_cast<double>(shown) / static_cast<double>(count), 0);
    }
    if (app.dataset->column_count() > 0) {
        right += "   " + sanitize(app.dataset->columns()[app.column].name);
    }
    draw_right(screen, y, screen.width() - 2, right, Style::fg_bg(kMuted, kBarBg));
    const int left_room = std::max(0, screen.width() - 4 - text_width(right) - 2);

    int x = 2;
    if (app.filter_active) {
        const std::string chip = "≡ " + app.filter_label;
        screen.put(x, y, truncate_width(chip, std::max(4, left_room / 2)), Style::fg_bg(kAccent, kBarBg));
        x += std::min(left_room / 2, text_width(truncate_width(chip, std::max(4, left_room / 2))));
        x += 2;
    }
    if (app.sort_label.size() > 0) {
        const std::string chip = "↕ " + app.sort_label;
        if (x < screen.width() - text_width(right) - 8) {
            screen.put(x, y, truncate_width(chip, std::max(4, left_room / 2)), Style::fg_bg(kMuted, kBarBg));
            x += text_width(truncate_width(chip, std::max(4, left_room / 2)));
            x += 2;
        }
    }
    if (!app.filter_active && app.sort_label.empty() && app.mode == Mode::Browse) {
        screen.put(x, y, "all rows", Style::fg_bg(kFaint, kBarBg));
    }
}

struct KeyHint {
    std::string_view key;
    std::string_view label;
};

void draw_hints(App& app) {
    Screen& screen = app.screen;
    const int y = screen.height() - 1;
    draw_bar(screen, y, Style::fg_bg(kMuted, kBarBg));

    if (app.toast.visible(now_seconds())) {
        const Style style = Style::fg_bg(app.toast.error ? kError : kSuccess, kBarBg).bold();
        screen.put(2, y, app.toast.error ? "! " : "✓ ", style);
        screen.put(4, y, truncate_width(app.toast.text, screen.width() - 6), style);
        return;
    }

    static constexpr KeyHint browse[] = {
        {"←→", "column"}, {"↑↓", "row"},   {"pgup/pgdn", "page"}, {"/", "search"}, {"f", "filter"},
        {"s", "sort"},    {"p", "profile"}, {"↵", "record"},      {"e", "export"}, {"?", "help"},
        {"q", "quit"},
    };
    static constexpr KeyHint prompt[] = {
        {"↵", "apply"}, {"esc", "cancel"}, {"tab", "complete"}, {"ctrl-u", "clear"},
    };
    static constexpr KeyHint help[] = {{"esc", "close"}, {"any key", "close"}};
    static constexpr KeyHint pager[] = {{"↑↓", "scroll"}, {"←→", "column"}, {"esc", "close"}};
    static constexpr KeyHint menu[] = {{"↑↓", "choose"}, {"↵", "apply"}, {"esc", "cancel"}};

    const KeyHint* hints = browse;
    std::size_t hint_count = std::size(browse);
    switch (app.mode) {
        case Mode::Filter:
        case Mode::Command:
        case Mode::Export: hints = prompt; hint_count = std::size(prompt); break;
        case Mode::Help: hints = help; hint_count = std::size(help); break;
        case Mode::Profile:
        case Mode::Record: hints = pager; hint_count = std::size(pager); break;
        case Mode::Sort: hints = menu; hint_count = std::size(menu); break;
        default: break;
    }

    int x = 2;
    for (std::size_t i = 0; i < hint_count && x < screen.width() - 4; ++i) {
        const int key_width = text_width(hints[i].key);
        const int label_width = text_width(hints[i].label);
        if (x + key_width + 1 + label_width > screen.width() - 2) break;
        screen.put(x, y, hints[i].key, Style::fg_bg(kStrong, kBarBg).bold());
        x += key_width + 1;
        screen.put(x, y, hints[i].label, Style::fg_bg(kFaint, kBarBg));
        x += label_width + 2;
        if (i + 1 < hint_count && x < screen.width() - 6) {
            screen.put(x, y, "·", Style::fg_bg(kBorder, kBarBg));
            x += 2;
        }
    }
}

void draw_prompt(App& app) {
    Screen& screen = app.screen;
    const int y = screen.height() - 3;
    if (y < 4) return;
    for (int x = 0; x < screen.width(); ++x) screen.put(x, y, " ", Style::fg_bg(kText, kPanelBg));
    screen.put(0, y, "▌", Style::fg_bg(kAccent, kPanelBg));

    int x = 2;
    const std::string chip = " " + app.prompt_title + " ";
    screen.put(x, y, chip, Style::fg_bg(kPageBg, kAccent).bold());
    x += static_cast<int>(text_width(chip)) + 2;

    const char* hint = "";
    switch (app.mode) {
        case Mode::Filter: hint = "enter apply · esc cancel"; break;
        case Mode::Command: hint = "enter run · esc cancel"; break;
        case Mode::Export: hint = "enter write file · esc cancel"; break;
        default: hint = ""; break;
    }
    const int hint_width = static_cast<int>(text_width(hint));
    if (!app.prompt_error.empty()) {
        draw_right(screen, y, screen.width() - 2, truncate_width(app.prompt_error, screen.width() / 2),
                   Style::fg_bg(kError, kPanelBg).bold());
    } else if (*hint != '\0') {
        draw_right(screen, y, screen.width() - 2, hint, Style::fg_bg(kMuted, kPanelBg));
    }

    const int room = std::max(4, screen.width() - x - hint_width - 6);
    // Scroll horizontally so the caret is always visible.
    std::size_t start = 0;
    if (app.prompt_cursor > static_cast<std::size_t>(room)) {
        start = app.prompt_cursor - static_cast<std::size_t>(room);
    }
    const std::size_t caret = std::min(app.prompt_cursor, app.prompt.size());
    const std::string before = truncate_width(
        std::string_view(app.prompt).substr(start, caret - start), room);
    screen.put(x, y, before, Style::fg_bg(kStrong, kPanelBg).bold());
    const int caret_x = x + text_width(before);
    std::string at = caret < app.prompt.size() ? app.prompt.substr(caret, 1) : " ";
    screen.put(caret_x, y, at, Style::fg_bg(kPageBg, kAccent).bold());
    const std::size_t tail_start = std::min(app.prompt.size(), caret + 1);
    const std::string tail = truncate_width(std::string_view(app.prompt).substr(tail_start),
                                            std::max(0, room - text_width(before)));
    screen.put(caret_x + 1, y, tail, Style::fg_bg(kMuted, kPanelBg));
}

} // namespace

void render_frame(App& app) {
    Screen& screen = app.screen;
    if (screen.width() != app.terminal.width() || screen.height() != app.terminal.height()) {
        screen.resize(app.terminal.width(), app.terminal.height());
        screen.invalidate();
        app.ensure_visible();
    }

    screen.clear(theme::base());
    const GridLayout grid = app.layout();
    draw_brand_bar(app);
    draw_column_header(app, grid);
    draw_grid(app, grid);
    draw_status(app, grid);
    draw_hints(app);
    if (app.mode == Mode::Filter || app.mode == Mode::Command || app.mode == Mode::Export) draw_prompt(app);

    switch (app.mode) {
        case Mode::Help: draw_help(app); break;
        case Mode::Sort: draw_sort_menu(app); break;
        case Mode::Profile: draw_profile(app); break;
        case Mode::Record: draw_record(app); break;
        default: break;
    }
    if (app.busy()) draw_progress(app);

    app.terminal.write(screen.flush());
}

} // namespace tess::tui
