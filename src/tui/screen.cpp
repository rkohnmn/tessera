// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · screen.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "tui/screen.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <charconv>

namespace tess::tui {
namespace {

void append_cursor(std::string& out, int x, int y) {
    out.append("\x1b[");
    out.append(std::to_string(y + 1));
    out.push_back(';');
    out.append(std::to_string(x + 1));
    out.push_back('H');
}

/// Emits one SGR sequence. Building into a std::string rather than a fixed
/// buffer keeps the compiler (and future maintainers) from having to reason
/// about the maximum length of a colour + attribute combination.
void append_style(std::string& out, const Style& style) {
    out.append("\x1b[");
    bool first = true;
    const auto code = [&](std::string_view value) {
        if (!first) out.push_back(';');
        out.append(value);
        first = false;
    };
    if (style.fg >= 0) {
        code("38;5");
        code(std::to_string(style.fg)); // 0..255, at most three digits
    }
    if (style.bg >= 0) {
        code("48;5");
        code(std::to_string(style.bg));
    }
    if ((style.attrs & 1u) != 0) code("1");
    if ((style.attrs & 2u) != 0) code("2");
    if ((style.attrs & 4u) != 0) code("3");
    if ((style.attrs & 8u) != 0) code("4");
    if ((style.attrs & 16u) != 0) code("7");
    out.push_back('m');
}

} // namespace

Screen::Screen(int width, int height) { resize(width, height); }

void Screen::resize(int width, int height) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    cells_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), Cell{});
    previous_.assign(cells_.size(), Cell{});
    full_redraw_ = true;
}

void Screen::invalidate() {
    full_redraw_ = true;
    previous_.assign(cells_.size(), Cell{});
}

Cell* Screen::at(int x, int y) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return nullptr;
    return &cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

const Cell* Screen::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) return nullptr;
    return &cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

void Screen::clear(const Style& style) {
    for (auto& cell : cells_) {
        cell.text.assign(" ");
        cell.style = style;
        cell.shadow = false;
    }
}

void Screen::fill(int x, int y, int w, int h, char32_t codepoint, const Style& style) {
    std::string glyph;
    // Encode the codepoint once.
    if (codepoint < 0x80) {
        glyph.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        glyph.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        glyph.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        glyph.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        glyph.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        glyph.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    for (int row = y; row < y + h; ++row) {
        for (int col = x; col < x + w; ++col) {
            Cell* cell = at(col, row);
            if (cell == nullptr) continue;
            cell->text = glyph;
            cell->style = style;
            cell->shadow = false;
        }
    }
}

void Screen::put_char(int x, int y, char32_t codepoint, const Style& style) {
    Cell* cell = at(x, y);
    if (cell == nullptr) return;
    if (cell->shadow) {
        // Overwriting the right half of a wide glyph: blank its left half so a
        // stray fragment is never left behind.
        if (Cell* left = at(x - 1, y)) {
            left->text = " ";
            left->style = style;
            left->shadow = false;
        }
    }
    std::string glyph;
    if (codepoint < 0x80) {
        glyph.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        glyph.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        glyph.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
        glyph.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        glyph.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        glyph.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        glyph.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        glyph.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        glyph.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        glyph.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    const int width = rune_width(codepoint);
    cell->text = std::move(glyph);
    cell->style = style;
    cell->shadow = false;
    if (width == 2) {
        if (Cell* right = at(x + 1, y)) {
            right->text.clear();
            right->style = style;
            right->shadow = true;
        }
    }
}

void Screen::put(int x, int y, std::string_view text, const Style& style) {
    int column = x;
    std::size_t index = 0;
    while (index < text.size() && column < width_) {
        const Rune rune = decode_rune(text, index);
        index += static_cast<std::size_t>(rune.length);
        if (rune.codepoint < 32 || (rune.codepoint >= 0x7F && rune.codepoint < 0xA0)) {
            // Control characters never reach the terminal from data files.
            put_char(column, y, 0x00B7, style);
            ++column;
            continue;
        }
        const int width = rune_width(rune.codepoint);
        if (width == 0) {
            // Combining mark: append to the previous cell.
            if (Cell* previous = at(column - 1, y)) {
                previous->text.append(text.substr(index - static_cast<std::size_t>(rune.length),
                                                  static_cast<std::size_t>(rune.length)));
            }
            continue;
        }
        if (column + width > width_) break;
        put_char(column, y, rune.codepoint, style);
        column += width;
    }
}

void Screen::hline(int x, int y, int length, std::string_view glyph, const Style& style) {
    for (int i = 0; i < length; ++i) put(x + i, y, glyph, style);
}

void Screen::vline(int x, int y, int length, std::string_view glyph, const Style& style) {
    for (int i = 0; i < length; ++i) put(x, y + i, glyph, style);
}

void Screen::box(int x, int y, int w, int h, const Style& style, bool rounded) {
    if (w < 2 || h < 2) return;
    const std::string_view horizontal = "─";
    const std::string_view vertical = "│";
    const std::string_view top_left = rounded ? "╭" : "┌";
    const std::string_view top_right = rounded ? "╮" : "┐";
    const std::string_view bottom_left = rounded ? "╰" : "└";
    const std::string_view bottom_right = rounded ? "╯" : "┘";

    put(x, y, top_left, style);
    hline(x + 1, y, w - 2, horizontal, style);
    put(x + w - 1, y, top_right, style);

    vline(x, y + 1, h - 2, vertical, style);
    vline(x + w - 1, y + 1, h - 2, vertical, style);

    put(x, y + h - 1, bottom_left, style);
    hline(x + 1, y + h - 1, w - 2, horizontal, style);
    put(x + w - 1, y + h - 1, bottom_right, style);
}

std::string Screen::flush() {
    std::string out;
    out.reserve(static_cast<std::size_t>(width_) * 4 + 64);

    Style current;
    bool have_style = false;
    const std::size_t total = cells_.size();
    for (std::size_t index = 0; index < total; ++index) {
        const Cell& cell = cells_[index];
        const Cell& previous = previous_[index];
        if (!full_redraw_ && cell.shadow == previous.shadow && cell.text == previous.text &&
            cell.style == previous.style) {
            continue;
        }
        if (cell.shadow) continue;

        const int y = static_cast<int>(index / static_cast<std::size_t>(width_));
        const int x = static_cast<int>(index % static_cast<std::size_t>(width_));
        append_cursor(out, x, y);
        if (!have_style || !(cell.style == current)) {
            append_style(out, cell.style);
            current = cell.style;
            have_style = true;
        }
        if (cell.text.empty()) {
            out.push_back(' ');
        } else {
            out.append(cell.text);
        }
    }
    if (have_style) out.append("\x1b[0m");
    previous_ = cells_;
    full_redraw_ = false;
    return out;
}

} // namespace tess::tui
