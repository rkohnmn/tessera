// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · util.hpp
//  Locale-independent numeric parsing/formatting, UTF-8 helpers and text
//  measurement. Everything here is allocation-light on the hot path because
//  cell rendering calls into it thousands of times per frame.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tess {

// ── Strings ──────────────────────────────────────────────────────────────────
[[nodiscard]] std::string_view trim(std::string_view s) noexcept;
[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool starts_with_icase(std::string_view s, std::string_view prefix) noexcept;
[[nodiscard]] std::string to_lower(std::string_view s);
[[nodiscard]] std::string to_upper(std::string_view s);
[[nodiscard]] bool contains_icase(std::string_view haystack, std::string_view needle) noexcept;
[[nodiscard]] std::string_view lstrip(std::string_view s) noexcept;
[[nodiscard]] std::string_view rstrip(std::string_view s) noexcept;

// ── Numbers ──────────────────────────────────────────────────────────────────
/// Integer parse: optional +/- sign, digits only, no separators. Surrounding
/// ASCII whitespace is ignored, because CSV exports frequently include it.
[[nodiscard]] bool parse_int(std::string_view s, std::int64_t& out) noexcept;
/// Locale-independent decimal parse. Accepts 1e9, .5, -2., +/-inf and nan.
[[nodiscard]] bool parse_double(std::string_view s, double& out) noexcept;
[[nodiscard]] bool parse_bool(std::string_view s, bool& out) noexcept;

/// Human formatting: 1234567 -> "1,234,567"; 1536 -> "1.5 KB".
[[nodiscard]] std::string format_int(std::int64_t v);
[[nodiscard]] std::string format_count(std::uint64_t v);
[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
/// Shortest representation that round-trips for display purposes (6 sig figs).
[[nodiscard]] std::string format_double(double v, int max_significant = 6);
[[nodiscard]] std::string format_percent(double fraction, int decimals = 1);

// ── Time ────────────────────────────────────────────────────────────────────
/// Parses ISO-8601 dates and timestamps ("2024-01-31", "2024-01-31 10:22:03",
/// "2024-01-31T10:22:03.512Z", "2024-01-31T10:22:03+02:00") to Unix seconds.
/// Date-only strings are accepted and interpreted as UTC midnight.
[[nodiscard]] std::optional<std::int64_t> parse_timestamp(std::string_view s) noexcept;
/// Formats Unix seconds as "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS" (UTC).
[[nodiscard]] std::string format_timestamp(std::int64_t epoch_seconds, bool date_only = false);
[[nodiscard]] double now_seconds() noexcept;

// ── UTF-8 / terminal text ───────────────────────────────────────────────────
struct Rune {
    char32_t codepoint = 0;
    int length = 1; ///< bytes consumed (>= 1)
    bool valid = false;
};
[[nodiscard]] Rune decode_rune(std::string_view s, std::size_t offset) noexcept;

/// Terminal column width of a codepoint: 0 (combining), 1 or 2 (CJK/emoji).
[[nodiscard]] int rune_width(char32_t cp) noexcept;
/// Display width of a UTF-8 string. Control bytes count as one column each,
/// which is what the renderer paints for them.
[[nodiscard]] int text_width(std::string_view s) noexcept;
/// Number of display cells' worth of runes (not bytes, not columns).
[[nodiscard]] std::size_t rune_count(std::string_view s) noexcept;

/// Cuts `s` to at most `width` display columns, appending '…' when truncated.
[[nodiscard]] std::string truncate_width(std::string_view s, int width, bool ellipsis = true);
/// Writes `s` into `out`, replacing control/escape bytes with '·' so hostile
/// data cannot drive the terminal. Copies at most `max_width` columns.
void append_safe(std::string& out, std::string_view s, int max_width);
/// Replaces every control byte and invalid sequence with U+00B7 middle dot.
[[nodiscard]] std::string sanitize(std::string_view s);
/// Shortens a path for a status bar, keeping the file name and parent dir.
[[nodiscard]] std::string ellipsize_path(std::string_view path, int width);

/// Locale-independent, allocation-free integer to text.
void append_int(std::string& out, std::int64_t v);
void append_count(std::string& out, std::uint64_t v);
void append_bytes(std::string& out, std::uint64_t bytes);
/// Appends `v` padded with thousands separators inside `width` columns.
void append_aligned_number(std::string& out, std::string_view text, int width);

// ── File system ─────────────────────────────────────────────────────────────
//  Every path in Tessera is UTF-8, because that is what the command line and the
//  terminal give us. The Windows CRT's narrow file APIs (`fopen`, `ifstream`,
//  `std::filesystem`) interpret bytes in the *ANSI code page* instead, so a path
//  with an accent, a CJK character or an emoji silently fails to open — or, far
//  worse, opens a file with a mojibake name. These helpers convert to UTF-16 and
//  call the wide APIs, so non-ASCII paths behave the same on every platform.

/// `fopen` with a UTF-8 path. Returns nullptr on failure, like `fopen`.
[[nodiscard]] std::FILE* fopen_utf8(const std::string& path, const char* mode);

/// True when `path` exists and is not a directory.
[[nodiscard]] bool file_exists(const std::string& path);

/// One-time process setup before any output is written.
///
/// On Windows a console interprets output bytes using its *code page*, which is
/// CP437 or CP1252 by default — so box-drawing characters, `◆`, `·`, CJK text
/// and accented names arrive as mojibake unless the code page is UTF-8. The same
/// console also prints ANSI escape sequences literally unless virtual-terminal
/// processing is switched on. Both are enabled here, once, so the CLI verbs and
/// the explorer render identically to Linux and macOS. A no-op elsewhere.
void configure_console();

#if defined(TESSERA_WINDOWS)
/// UTF-8 -> UTF-16. Throws tess::Error when the input is not valid UTF-8.
[[nodiscard]] std::wstring utf8_to_wide(const std::string& text);
#endif

// ── Misc ────────────────────────────────────────────────────────────────────
[[nodiscard]] std::vector<std::string_view> split(std::string_view s, char sep);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view sep);
[[nodiscard]] std::size_t clamp_size(std::size_t v, std::size_t lo, std::size_t hi) noexcept;

} // namespace tess
