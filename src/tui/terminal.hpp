// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · terminal.hpp
//  Raw-mode terminal I/O with a single interface over POSIX termios and the
//  Win32 console. Keys, resize, wheel and click all arrive as KeyEvents; the
//  application loop never talks to the platform directly.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <deque>
#include <string>
#include <string_view>

namespace tess::tui {

enum class Key {
    None,
    Char,
    Enter,
    Esc,
    Tab,
    Backspace,
    Delete,
    Insert,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    Ctrl,
    Resize,
    MouseClick,
    MouseScrollUp,
    MouseScrollDown,
    Paste,
    Unknown,
};

struct KeyEvent {
    Key key = Key::None;
    char32_t ch = 0;      ///< for Key::Char and Key::Ctrl ('a'..'z')
    std::string text;     ///< UTF-8 bytes for paste
    int mouse_x = 0;
    int mouse_y = 0;

    [[nodiscard]] bool is(char32_t codepoint) const { return key == Key::Char && ch == codepoint; }
    [[nodiscard]] bool is_alpha(char32_t lowered) const {
        if (key != Key::Char) return false;
        char32_t c = ch;
        if (c >= 'A' && c <= 'Z') c = static_cast<char32_t>(c - 'A' + 'a');
        return c == lowered;
    }
};

class Terminal {
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// Switches to the alternate screen and raw mode. Throws tess::Error when
    /// stdin is not a terminal.
    void init();
    /// Restores the previous terminal state. Safe to call twice.
    void shutdown();

    /// Waits up to `timeout_ms` for one event. Returns false on timeout.
    [[nodiscard]] bool read_key(KeyEvent& event, int timeout_ms);

    [[nodiscard]] int width() const;
    [[nodiscard]] int height() const;
    void write(std::string_view bytes);
    void set_title(std::string_view title);
    /// OSC 52 clipboard copy (works through SSH and Windows Terminal).
    void copy_to_clipboard(std::string_view text);

private:
    void push_bytes(std::string_view bytes);
    [[nodiscard]] bool parse_pending(KeyEvent& event);
    [[nodiscard]] int platform_width() const;
    [[nodiscard]] int platform_height() const;

    std::string pending_;
    std::deque<KeyEvent> queued_;
    bool active_ = false;
    int cached_width_ = 100;
    int cached_height_ = 30;

#if defined(TESSERA_WINDOWS)
    void* input_handle_ = nullptr;
    void* output_handle_ = nullptr;
    unsigned long saved_input_mode_ = 0;
    unsigned long saved_output_mode_ = 0;
#else
    struct TermiosState;
    TermiosState* state_ = nullptr;
    bool saw_resize_ = false;
#endif
};

} // namespace tess::tui
