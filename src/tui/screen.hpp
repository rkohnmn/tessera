// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · screen.hpp
//  A double-buffered character grid. Every frame is diffed against the last and
//  emitted as one write() containing only the cells that changed, which is what
//  keeps scrolling smooth on a 240-column terminal over a remote connection.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tess::tui {

struct Style {
    /// 256-colour index, or -1 for the terminal default.
    std::int16_t fg = -1;
    std::int16_t bg = -1;
    std::uint8_t attrs = 0; ///< 1 bold, 2 dim, 4 italic, 8 underline, 16 reverse

    [[nodiscard]] static Style fg_bg(int fg_color, int bg_color = -1, std::uint8_t attributes = 0) {
        Style style;
        style.fg = static_cast<std::int16_t>(fg_color);
        style.bg = static_cast<std::int16_t>(bg_color);
        style.attrs = attributes;
        return style;
    }
    [[nodiscard]] Style bold() const {
        Style copy = *this;
        copy.attrs = static_cast<std::uint8_t>(copy.attrs | 1u);
        return copy;
    }
    [[nodiscard]] Style dim() const {
        Style copy = *this;
        copy.attrs = static_cast<std::uint8_t>(copy.attrs | 2u);
        return copy;
    }
    [[nodiscard]] Style underline() const {
        Style copy = *this;
        copy.attrs = static_cast<std::uint8_t>(copy.attrs | 8u);
        return copy;
    }
    [[nodiscard]] Style reversed() const {
        Style copy = *this;
        copy.attrs = static_cast<std::uint8_t>(copy.attrs | 16u);
        return copy;
    }
    bool operator==(const Style&) const = default;
};

struct Cell {
    std::string text;
    Style style;
    bool shadow = false; ///< right half of a wide glyph; not painted directly
};

class Screen {
public:
    Screen(int width, int height);

    void resize(int width, int height);
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    void clear(const Style& style = {});
    void fill(int x, int y, int w, int h, char32_t codepoint, const Style& style);
    void put(int x, int y, std::string_view text, const Style& style);
    void put_char(int x, int y, char32_t codepoint, const Style& style);
    void hline(int x, int y, int length, std::string_view glyph, const Style& style);
    void vline(int x, int y, int length, std::string_view glyph, const Style& style);
    void box(int x, int y, int w, int h, const Style& style, bool rounded = true);

    /// Forces the next flush to repaint everything (after resize or alt-screen
    /// entry, when the terminal contents are unknown).
    void invalidate();

    /// Returns the escape sequence for the cells that changed since last call.
    [[nodiscard]] std::string flush();

private:
    [[nodiscard]] Cell* at(int x, int y);
    [[nodiscard]] const Cell* at(int x, int y) const;

    int width_ = 0;
    int height_ = 0;
    std::vector<Cell> cells_;
    std::vector<Cell> previous_;
    bool full_redraw_ = true;
};

} // namespace tess::tui
