// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · terminal.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "tui/terminal.hpp"

#include "common/error.hpp"
#include "common/util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(TESSERA_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <csignal>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace tess::tui {
namespace {

std::string base64(std::string_view input) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < input.size()) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16) |
                                     (static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8) |
                                     static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 2]));
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(kTable[(triple >> 6) & 0x3F]);
        out.push_back(kTable[triple & 0x3F]);
        i += 3;
    }
    if (i < input.size()) {
        std::uint32_t triple = static_cast<std::uint32_t>(static_cast<unsigned char>(input[i])) << 16;
        if (i + 1 < input.size()) {
            triple |= static_cast<std::uint32_t>(static_cast<unsigned char>(input[i + 1])) << 8;
        }
        out.push_back(kTable[(triple >> 18) & 0x3F]);
        out.push_back(kTable[(triple >> 12) & 0x3F]);
        out.push_back(i + 1 < input.size() ? kTable[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

#if !defined(TESSERA_WINDOWS)
volatile std::sig_atomic_t g_resized = 0;

extern "C" void on_sigwinch(int) { g_resized = 1; }
#endif

} // namespace

#if !defined(TESSERA_WINDOWS)
struct Terminal::TermiosState {
    struct termios saved {};
};
#endif

Terminal::Terminal() = default;

Terminal::~Terminal() { shutdown(); }

void Terminal::init() {
    if (active_) return;
#if defined(TESSERA_WINDOWS)
    auto* input = static_cast<HANDLE>(GetStdHandle(STD_INPUT_HANDLE));
    auto* output = static_cast<HANDLE>(GetStdHandle(STD_OUTPUT_HANDLE));
    if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
        fail("the interactive explorer needs a terminal; try 'tessera head <file>'");
    }
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    if (!GetConsoleMode(input, &input_mode) || !GetConsoleMode(output, &output_mode)) {
        fail("the interactive explorer needs a console; try 'tessera head <file>'");
    }
    input_handle_ = input;
    output_handle_ = output;
    saved_input_mode_ = input_mode;
    saved_output_mode_ = output_mode;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(output, output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    SetConsoleMode(input, (input_mode & ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                          ENABLE_PROCESSED_INPUT)) |
                              ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fail("the interactive explorer needs a terminal; try 'tessera head <file>'");
    }
    state_ = new TermiosState();
    if (tcgetattr(STDIN_FILENO, &state_->saved) != 0) {
        delete state_;
        state_ = nullptr;
        fail("cannot read terminal attributes");
    }
    struct termios raw = state_->saved;
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        delete state_;
        state_ = nullptr;
        fail("cannot switch the terminal into raw mode");
    }
    struct sigaction action {};
    action.sa_handler = on_sigwinch;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGWINCH, &action, nullptr);
#endif
    active_ = true;
    // Alternate screen, hidden cursor, SGR mouse, bracketed paste.
    write("\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1006h\x1b[?2004h\x1b[2J\x1b[H");
}

void Terminal::shutdown() {
    if (!active_) return;
    write("\x1b[?2004l\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l");
#if defined(TESSERA_WINDOWS)
    if (input_handle_ != nullptr) SetConsoleMode(static_cast<HANDLE>(input_handle_), saved_input_mode_);
    if (output_handle_ != nullptr) SetConsoleMode(static_cast<HANDLE>(output_handle_), saved_output_mode_);
    input_handle_ = nullptr;
    output_handle_ = nullptr;
#else
    if (state_ != nullptr) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &state_->saved);
        delete state_;
        state_ = nullptr;
    }
#endif
    active_ = false;
}

void Terminal::write(std::string_view bytes) {
    if (bytes.empty()) return;
#if defined(TESSERA_WINDOWS)
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
#else
    const char* data = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t written = ::write(STDOUT_FILENO, data, remaining);
        if (written <= 0) break;
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
#endif
}

int Terminal::platform_width() const {
#if defined(TESSERA_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    auto* output = static_cast<HANDLE>(output_handle_ != nullptr ? output_handle_ : GetStdHandle(STD_OUTPUT_HANDLE));
    if (GetConsoleScreenBufferInfo(output, &info)) {
        return static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1);
    }
    return 100;
#else
    struct winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) return static_cast<int>(size.ws_col);
    return 100;
#endif
}

int Terminal::platform_height() const {
#if defined(TESSERA_WINDOWS)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    auto* output = static_cast<HANDLE>(output_handle_ != nullptr ? output_handle_ : GetStdHandle(STD_OUTPUT_HANDLE));
    if (GetConsoleScreenBufferInfo(output, &info)) {
        return static_cast<int>(info.srWindow.Bottom - info.srWindow.Top + 1);
    }
    return 30;
#else
    struct winsize size {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0) return static_cast<int>(size.ws_row);
    return 30;
#endif
}

int Terminal::width() const { return platform_width(); }

int Terminal::height() const { return platform_height(); }

void Terminal::set_title(std::string_view title) {
    std::string sequence = "\x1b]0;";
    sequence.append(title);
    sequence.push_back('\a');
    write(sequence);
}

void Terminal::copy_to_clipboard(std::string_view text) {
    std::string sequence = "\x1b]52;c;";
    sequence += base64(text);
    sequence.push_back('\a');
    write(sequence);
}

#if defined(TESSERA_WINDOWS)
namespace {

KeyEvent translate_key(const KEY_EVENT_RECORD& record) {
    KeyEvent event;
    if (record.bKeyDown == FALSE) return event;
    const WORD vk = record.wVirtualKeyCode;
    const DWORD modifiers = record.dwControlKeyState;
    const bool ctrl = (modifiers & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    const bool alt = (modifiers & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

    switch (vk) {
        case VK_UP: event.key = Key::Up; return event;
        case VK_DOWN: event.key = Key::Down; return event;
        case VK_LEFT: event.key = Key::Left; return event;
        case VK_RIGHT: event.key = Key::Right; return event;
        case VK_HOME: event.key = Key::Home; return event;
        case VK_END: event.key = Key::End; return event;
        case VK_PRIOR: event.key = Key::PageUp; return event;
        case VK_NEXT: event.key = Key::PageDown; return event;
        case VK_DELETE: event.key = Key::Delete; return event;
        case VK_INSERT: event.key = Key::Insert; return event;
        case VK_BACK: event.key = Key::Backspace; return event;
        case VK_RETURN: event.key = Key::Enter; return event;
        case VK_TAB: event.key = Key::Tab; return event;
        case VK_ESCAPE: event.key = Key::Esc; return event;
        case VK_F1: event.key = Key::F1; return event;
        case VK_F2: event.key = Key::F2; return event;
        case VK_F3: event.key = Key::F3; return event;
        case VK_F4: event.key = Key::F4; return event;
        case VK_F5: event.key = Key::F5; return event;
        case VK_F6: event.key = Key::F6; return event;
        case VK_F7: event.key = Key::F7; return event;
        case VK_F8: event.key = Key::F8; return event;
        case VK_F9: event.key = Key::F9; return event;
        case VK_F10: event.key = Key::F10; return event;
        case VK_F11: event.key = Key::F11; return event;
        case VK_F12: event.key = Key::F12; return event;
        default: break;
    }
    if (record.uChar.UnicodeChar == 0) return event;
    const char32_t ch = record.uChar.UnicodeChar;
    if (ctrl) {
        event.key = Key::Ctrl;
        event.ch = ch;
        return event;
    }
    if (alt) {
        event.key = Key::Char;
        event.ch = ch;
        return event;
    }
    if (ch >= 32) {
        event.key = Key::Char;
        event.ch = ch;
    }
    return event;
}

} // namespace

bool Terminal::read_key(KeyEvent& event, int timeout_ms) {
    if (!queued_.empty()) {
        event = queued_.front();
        queued_.pop_front();
        return true;
    }
    auto* input = static_cast<HANDLE>(input_handle_);
    if (input == nullptr) return false;
    if (WaitForSingleObject(input, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) return false;

    INPUT_RECORD records[64];
    DWORD count = 0;
    if (!ReadConsoleInputW(input, records, 64, &count)) return false;
    for (DWORD i = 0; i < count; ++i) {
        const INPUT_RECORD& record = records[i];
        if (record.EventType == KEY_EVENT) {
            const KeyEvent translated = translate_key(record.Event.KeyEvent);
            if (translated.key != Key::None) queued_.push_back(translated);
        } else if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            KeyEvent resize;
            resize.key = Key::Resize;
            queued_.push_back(resize);
        } else if (record.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
            KeyEvent mouse_event;
            mouse_event.mouse_x = mouse.dwMousePosition.X;
            mouse_event.mouse_y = mouse.dwMousePosition.Y;
            if ((mouse.dwEventFlags & MOUSE_WHEELED) != 0) {
                const short delta = static_cast<short>(HIWORD(mouse.dwButtonState));
                mouse_event.key = delta > 0 ? Key::MouseScrollUp : Key::MouseScrollDown;
                queued_.push_back(mouse_event);
            } else if (mouse.dwEventFlags == 0 && (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0) {
                mouse_event.key = Key::MouseClick;
                queued_.push_back(mouse_event);
            }
        }
    }
    if (queued_.empty()) return false;
    event = queued_.front();
    queued_.pop_front();
    return true;
}

#else

void Terminal::push_bytes(std::string_view bytes) { pending_.append(bytes); }

bool Terminal::parse_pending(KeyEvent& event) {
    while (!pending_.empty()) {
        const auto first = static_cast<unsigned char>(pending_[0]);

        if (first == 0x1b) {
            if (pending_.size() == 1) return false;
            const char second = pending_[1];
            if (second == '[') {
                std::size_t i = 2;
                std::string params;
                while (i < pending_.size() && (static_cast<unsigned char>(pending_[i]) < 0x40 ||
                                               static_cast<unsigned char>(pending_[i]) > 0x7E)) {
                    params.push_back(pending_[i]);
                    ++i;
                }
                if (i >= pending_.size()) return false; // incomplete
                const char final = pending_[i];
                pending_.erase(0, i + 1);

                if (final == 'M' || final == 'm') {
                    // SGR mouse:  <button;x;y
                    if (!params.empty() && params[0] == '<') {
                        int button = 0, x = 0, y = 0;
                        if (std::sscanf(params.c_str() + 1, "%d;%d;%d", &button, &x, &y) == 3) {
                            event = KeyEvent{};
                            event.mouse_x = x - 1;
                            event.mouse_y = y - 1;
                            if ((button & 0x40) != 0) {
                                event.key = (button & 1) != 0 ? Key::MouseScrollDown : Key::MouseScrollUp;
                            } else if (final == 'M' && (button & 3) == 0) {
                                event.key = Key::MouseClick;
                            } else {
                                continue;
                            }
                            return true;
                        }
                    }
                    continue;
                }
                event = KeyEvent{};
                switch (final) {
                    case 'A': event.key = Key::Up; return true;
                    case 'B': event.key = Key::Down; return true;
                    case 'C': event.key = Key::Right; return true;
                    case 'D': event.key = Key::Left; return true;
                    case 'H': event.key = Key::Home; return true;
                    case 'F': event.key = Key::End; return true;
                    case 'Z': event.key = Key::Tab; return true; // shift-tab, used for reverse tabbing
                    case '~': {
                        const int code = params.empty() ? 0 : std::atoi(params.c_str());
                        switch (code) {
                            case 1: event.key = Key::Home; return true;
                            case 2: event.key = Key::Insert; return true;
                            case 3: event.key = Key::Delete; return true;
                            case 4: event.key = Key::End; return true;
                            case 5: event.key = Key::PageUp; return true;
                            case 6: event.key = Key::PageDown; return true;
                            case 11: event.key = Key::F1; return true;
                            case 12: event.key = Key::F2; return true;
                            case 13: event.key = Key::F3; return true;
                            case 14: event.key = Key::F4; return true;
                            case 15: event.key = Key::F5; return true;
                            case 17: event.key = Key::F6; return true;
                            case 18: event.key = Key::F7; return true;
                            case 19: event.key = Key::F8; return true;
                            case 20: event.key = Key::F9; return true;
                            case 21: event.key = Key::F10; return true;
                            case 23: event.key = Key::F11; return true;
                            case 24: event.key = Key::F12; return true;
                            case 200:
                            case 201: continue; // bracketed paste markers
                            default: continue;
                        }
                    }
                    default: continue;
                }
            }
            if (second == 'O') {
                if (pending_.size() < 3) return false;
                const char code = pending_[2];
                pending_.erase(0, 3);
                event = KeyEvent{};
                switch (code) {
                    case 'P': event.key = Key::F1; return true;
                    case 'Q': event.key = Key::F2; return true;
                    case 'R': event.key = Key::F3; return true;
                    case 'S': event.key = Key::F4; return true;
                    case 'A': event.key = Key::Up; return true;
                    case 'B': event.key = Key::Down; return true;
                    case 'C': event.key = Key::Right; return true;
                    case 'D': event.key = Key::Left; return true;
                    case 'H': event.key = Key::Home; return true;
                    case 'F': event.key = Key::End; return true;
                    default: continue;
                }
            }
            // Alt + key.
            pending_.erase(0, 1);
            const Rune rune = decode_rune(pending_, 0);
            if (!rune.valid && !pending_.empty() && static_cast<unsigned char>(pending_[0]) >= 0x80 &&
                pending_.size() < 4) {
                pending_.insert(0, 1, '\x1b');
                return false;
            }
            pending_.erase(0, static_cast<std::size_t>(rune.length));
            event = KeyEvent{};
            event.key = Key::Char;
            event.ch = rune.codepoint;
            return true;
        }

        if (first == 0x0d || first == 0x0a) {
            pending_.erase(0, 1);
            event = KeyEvent{};
            event.key = Key::Enter;
            return true;
        }
        if (first == 0x09) {
            pending_.erase(0, 1);
            event = KeyEvent{};
            event.key = Key::Tab;
            return true;
        }
        if (first == 0x7f || first == 0x08) {
            pending_.erase(0, 1);
            event = KeyEvent{};
            event.key = Key::Backspace;
            return true;
        }
        if (first < 0x20) {
            pending_.erase(0, 1);
            event = KeyEvent{};
            event.key = Key::Ctrl;
            event.ch = (first == 0) ? U'@' : static_cast<char32_t>(first) + 0x60;
            return true;
        }

        const Rune rune = decode_rune(pending_, 0);
        if (!rune.valid) {
            if (pending_.size() < 4 && first >= 0xC0) return false; // possibly incomplete UTF-8
            pending_.erase(0, 1);
            event = KeyEvent{};
            event.key = Key::Unknown;
            return true;
        }
        pending_.erase(0, static_cast<std::size_t>(rune.length));
        event = KeyEvent{};
        event.key = Key::Char;
        event.ch = rune.codepoint;
        return true;
    }
    return false;
}

bool Terminal::read_key(KeyEvent& event, int timeout_ms) {
    if (!queued_.empty()) {
        event = queued_.front();
        queued_.pop_front();
        return true;
    }
    if (g_resized != 0) {
        g_resized = 0;
        event = KeyEvent{};
        event.key = Key::Resize;
        return true;
    }
    if (parse_pending(event)) return true;

    struct pollfd descriptor {};
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    const int ready = poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) {
        if (!pending_.empty() && pending_ == "\x1b") {
            pending_.clear();
            event = KeyEvent{};
            event.key = Key::Esc;
            return true;
        }
        return false;
    }
    char buffer[512];
    const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (got <= 0) return false;
    push_bytes(std::string_view(buffer, static_cast<std::size_t>(got)));
    if (parse_pending(event)) return true;
    // A complete event may not be available yet (split escape sequence).
    return false;
}

#endif

} // namespace tess::tui
