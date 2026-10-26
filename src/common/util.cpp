// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · util.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "common/util.hpp"

#include "common/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(TESSERA_WINDOWS)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace tess {
namespace {

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Powers of ten that are exactly representable as doubles (10^0 .. 10^22).
constexpr std::array<double, 23> kPow10 = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/// Howard Hinnant's civil-date algorithm: days since 1970-01-01.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= (m <= 2);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10u ? 3u : -9u);
    y += (m <= 2u);
}

bool read_uint(std::string_view s, std::size_t& i, int digits, unsigned& out) noexcept {
    if (i + static_cast<std::size_t>(digits) > s.size()) return false;
    unsigned v = 0;
    for (int k = 0; k < digits; ++k) {
        const char c = s[i + static_cast<std::size_t>(k)];
        if (c < '0' || c > '9') return false;
        v = v * 10u + static_cast<unsigned>(c - '0');
    }
    i += static_cast<std::size_t>(digits);
    out = v;
    return true;
}

} // namespace

// ── Strings ──────────────────────────────────────────────────────────────────
std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::string_view lstrip(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    return s;
}

std::string_view rstrip(std::string_view s) noexcept {
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool starts_with_icase(std::string_view s, std::string_view prefix) noexcept {
    if (s.size() < prefix.size()) return false;
    return iequals(s.substr(0, prefix.size()), prefix);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), ascii_lower);
    return out;
}

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

bool contains_icase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

// ── Numbers ──────────────────────────────────────────────────────────────────
bool parse_int(std::string_view s, std::int64_t& out) noexcept {
    s = trim(s);
    if (s.empty()) return false;
    bool negative = false;
    std::size_t i = 0;
    if (s[0] == '+' || s[0] == '-') {
        negative = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return false;
    // 19 digits always fit in int64; 20 may overflow, so use the exact limit.
    std::uint64_t value = 0;
    constexpr std::uint64_t kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const std::uint64_t limit = negative ? kMax + 1u : kMax;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (limit - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = negative ? static_cast<std::int64_t>(~value + 1u) : static_cast<std::int64_t>(value);
    return true;
}

bool parse_double(std::string_view s, double& out) noexcept {
    s = trim(s);
    if (s.empty()) return false;

    bool negative = false;
    std::size_t i = 0;
    if (s[i] == '+' || s[i] == '-') {
        negative = s[i] == '-';
        ++i;
    }
    if (i >= s.size()) return false;

    // Special values.
    const std::string_view rest = s.substr(i);
    if (iequals(rest, "inf") || iequals(rest, "infinity")) {
        out = negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
        return true;
    }
    if (iequals(rest, "nan")) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }

    std::uint64_t mantissa = 0;
    int exponent10 = 0;      // value = mantissa * 10^exponent10
    bool any_digit = false;
    bool truncated = false;

    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        any_digit = true;
        if (mantissa <= (std::numeric_limits<std::uint64_t>::max() - 9u) / 10u) {
            mantissa = mantissa * 10u + static_cast<std::uint64_t>(s[i] - '0');
        } else {
            ++exponent10; // keep magnitude, drop the least significant digits
            truncated = true;
        }
    }

    if (i < s.size() && s[i] == '.') {
        ++i;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            any_digit = true;
            if (mantissa <= (std::numeric_limits<std::uint64_t>::max() - 9u) / 10u) {
                mantissa = mantissa * 10u + static_cast<std::uint64_t>(s[i] - '0');
                --exponent10;
            } else {
                truncated = true; // already at max precision; ignore extra digits
            }
        }
    }

    if (!any_digit) return false;

    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool exp_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exp_negative = s[i] == '-';
            ++i;
        }
        if (i >= s.size()) return false;
        int exp_value = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            if (exp_value < 100000) exp_value = exp_value * 10 + (s[i] - '0');
        }
        exponent10 += exp_negative ? -exp_value : exp_value;
    }

    if (i != s.size()) return false;

    double value = static_cast<double>(mantissa);
    if (truncated) {
        // We dropped digits; scale back so the magnitude stays right.
        value = static_cast<double>(mantissa);
    }
    // Apply the decimal exponent in bounded steps to avoid overflow of the table.
    while (exponent10 > 0) {
        const int step = std::min(exponent10, 22);
        value *= kPow10[static_cast<std::size_t>(step)];
        exponent10 -= step;
        if (!std::isfinite(value)) break;
    }
    while (exponent10 < 0) {
        const int step = std::min(-exponent10, 22);
        value /= kPow10[static_cast<std::size_t>(step)];
        exponent10 += step;
        if (value == 0.0) break;
    }
    out = negative ? -value : value;
    return true;
}

bool parse_bool(std::string_view s, bool& out) noexcept {
    s = trim(s);
    if (iequals(s, "true") || iequals(s, "t") || iequals(s, "yes") || iequals(s, "y") ||
        iequals(s, "on") || s == "1") {
        out = true;
        return true;
    }
    if (iequals(s, "false") || iequals(s, "f") || iequals(s, "no") || iequals(s, "n") ||
        iequals(s, "off") || s == "0") {
        out = false;
        return true;
    }
    return false;
}

void append_int(std::string& out, std::int64_t v) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

namespace {
/// Groups digits of an unsigned decimal string with ',' separators.
void append_grouped(std::string& out, std::string_view digits) {
    const std::size_t n = digits.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
}
} // namespace

void append_count(std::string& out, std::uint64_t v) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    append_grouped(out, std::string_view(buf, static_cast<std::size_t>(res.ptr - buf)));
}

std::string format_int(std::int64_t v) {
    std::string out;
    out.reserve(24);
    if (v < 0) {
        out.push_back('-');
        append_count(out, static_cast<std::uint64_t>(-(v + 1)) + 1u);
    } else {
        append_count(out, static_cast<std::uint64_t>(v));
    }
    return out;
}

std::string format_count(std::uint64_t v) {
    std::string out;
    out.reserve(24);
    append_count(out, v);
    return out;
}

void append_bytes(std::string& out, std::uint64_t bytes) {
    static constexpr std::array<const char*, 5> kUnits = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) {
        std::snprintf(buf, sizeof(buf), "%llu %s", static_cast<unsigned long long>(bytes), kUnits[unit]);
    } else if (value < 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f %s", value, kUnits[unit]);
    }
    out.append(buf);
}

std::string format_bytes(std::uint64_t bytes) {
    std::string out;
    append_bytes(out, bytes);
    return out;
}

std::string format_double(double v, int max_significant) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    if (v == 0.0) return "0";
    max_significant = std::clamp(max_significant, 1, 17);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*g", max_significant, v);
    const double magnitude = std::fabs(v);
    // %g switches to exponent form aggressively; for values people read at a
    // glance (prices, counts, milliseconds) fixed notation is friendlier.
    if ((std::strchr(buf, 'e') != nullptr || std::strchr(buf, 'E') != nullptr) && magnitude >= 1e-4 &&
        magnitude < 1e7) {
        const int integer_digits = static_cast<int>(std::floor(std::log10(magnitude))) + 1;
        const int decimals = std::clamp(max_significant - integer_digits, 0, 17);
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        std::string text(buf);
        if (text.find('.') != std::string::npos) {
            while (!text.empty() && text.back() == '0') text.pop_back();
            if (!text.empty() && text.back() == '.') text.pop_back();
        }
        return text;
    }
    return std::string(buf);
}

std::string format_percent(double fraction, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f%%", decimals, fraction * 100.0);
    return std::string(buf);
}

void append_aligned_number(std::string& out, std::string_view text, int width) {
    const int len = static_cast<int>(text.size());
    if (len >= width) {
        out.append(text);
        return;
    }
    out.append(static_cast<std::size_t>(width - len), ' ');
    out.append(text);
}

// ── Time ────────────────────────────────────────────────────────────────────
std::optional<std::int64_t> parse_timestamp(std::string_view s) noexcept {
    s = trim(s);
    if (s.size() < 8) return std::nullopt;

    std::size_t i = 0;
    unsigned year = 0, month = 0, day = 0;
    if (!read_uint(s, i, 4, year)) return std::nullopt;
    if (i >= s.size()) return std::nullopt;
    const char sep = s[i];
    if (sep != '-' && sep != '/') return std::nullopt;
    ++i;
    if (!read_uint(s, i, 2, month)) return std::nullopt;
    if (i >= s.size() || s[i] != sep) return std::nullopt;
    ++i;
    if (!read_uint(s, i, 2, day)) return std::nullopt;

    if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

    unsigned hour = 0, minute = 0, second = 0;
    std::int64_t offset_seconds = 0;
    if (i < s.size()) {
        const char t = s[i];
        if (t != 'T' && t != 't' && t != ' ') return std::nullopt;
        ++i;
        if (!read_uint(s, i, 2, hour)) return std::nullopt;
        if (i >= s.size() || s[i] != ':') return std::nullopt;
        ++i;
        if (!read_uint(s, i, 2, minute)) return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!read_uint(s, i, 2, second)) return std::nullopt;
        }
        // Optional fractional seconds.
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        // Optional UTC designator or offset. The offset is applied, not
        // discarded: '2024-01-31T12:00:00+02:00' is 10:00 UTC, and users who
        // mix exports from several regions rely on that being true.
        if (i < s.size()) {
            if (s[i] == 'Z' || s[i] == 'z') {
                ++i;
            } else if (s[i] == '+' || s[i] == '-') {
                const bool west_of_utc = s[i] == '-';
                ++i;
                unsigned offset_hours = 0;
                unsigned offset_minutes = 0;
                if (!read_uint(s, i, 2, offset_hours)) return std::nullopt;
                if (i < s.size() && s[i] == ':') ++i;
                if (i + 2 <= s.size()) {
                    if (!read_uint(s, i, 2, offset_minutes)) return std::nullopt;
                }
                if (offset_hours > 23 || offset_minutes > 59) return std::nullopt;
                std::int64_t offset = static_cast<std::int64_t>(offset_hours) * 3600 +
                                      static_cast<std::int64_t>(offset_minutes) * 60;
                if (west_of_utc) offset = -offset;
                offset_seconds = offset;
            }
            if (i != s.size()) return std::nullopt;
        }
        if (hour > 23 || minute > 59 || second > 60) return std::nullopt;
    }

    const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), month, day);
    return days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
           static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second) - offset_seconds;
}

std::string format_timestamp(std::int64_t epoch_seconds, bool date_only) {
    std::int64_t days = epoch_seconds / 86400;
    std::int64_t rem = epoch_seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    std::int64_t year = 0;
    unsigned month = 1, day = 1;
    civil_from_days(days, year, month, day);

    char buf[40];
    if (date_only) {
        std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u", static_cast<long long>(year), month, day);
    } else {
        std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02lld:%02lld:%02lld",
                      static_cast<long long>(year), month, day,
                      static_cast<long long>(rem / 3600), static_cast<long long>((rem % 3600) / 60),
                      static_cast<long long>(rem % 60));
    }
    return std::string(buf);
}

double now_seconds() noexcept {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// ── UTF-8 / terminal text ───────────────────────────────────────────────────
Rune decode_rune(std::string_view s, std::size_t offset) noexcept {
    Rune r{};
    if (offset >= s.size()) return r;
    const auto byte = static_cast<unsigned char>(s[offset]);
    if (byte < 0x80) {
        r.codepoint = byte;
        r.length = 1;
        r.valid = true;
        return r;
    }
    int extra = 0;
    char32_t cp = 0;
    if ((byte & 0xE0) == 0xC0) {
        extra = 1;
        cp = byte & 0x1Fu;
    } else if ((byte & 0xF0) == 0xE0) {
        extra = 2;
        cp = byte & 0x0Fu;
    } else if ((byte & 0xF8) == 0xF0) {
        extra = 3;
        cp = byte & 0x07u;
    } else {
        r.codepoint = 0xFFFD;
        r.length = 1;
        return r;
    }
    if (offset + static_cast<std::size_t>(extra) >= s.size()) {
        r.codepoint = 0xFFFD;
        r.length = 1;
        return r;
    }
    for (int k = 1; k <= extra; ++k) {
        const auto cont = static_cast<unsigned char>(s[offset + static_cast<std::size_t>(k)]);
        if ((cont & 0xC0) != 0x80) {
            r.codepoint = 0xFFFD;
            r.length = 1;
            return r;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }
    r.codepoint = cp;
    r.length = extra + 1;
    r.valid = true;
    return r;
}

namespace {
struct Range {
    char32_t lo;
    char32_t hi;
};

constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4}, {0x17000, 0x18CFF}, {0x1B000, 0x1B2FF},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D},
    {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC},
    {0x1F6D0, 0x1F6D2}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

template <std::size_t N>
bool in_ranges(const Range (&ranges)[N], char32_t cp) noexcept {
    std::size_t lo = 0, hi = N;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (cp < ranges[mid].lo) {
            hi = mid;
        } else if (cp > ranges[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}
} // namespace

int rune_width(char32_t cp) noexcept {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0; // control: not displayed
    if (cp < 0x300) return 1;
    if (in_ranges(kZeroWidth, cp)) return 0;
    if (in_ranges(kWide, cp)) return 2;
    return 1;
}

int text_width(std::string_view s) noexcept {
    int width = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const Rune r = decode_rune(s, i);
        // Control bytes (including ESC) are painted as a single middle dot by
        // Screen::put, so layout must reserve one column for each of them.
        if (r.codepoint < 32 || (r.codepoint >= 0x7F && r.codepoint < 0xA0)) {
            ++width;
        } else {
            width += rune_width(r.codepoint);
        }
        i += static_cast<std::size_t>(r.length);
    }
    return width;
}

std::size_t rune_count(std::string_view s) noexcept {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        const Rune r = decode_rune(s, i);
        if (r.codepoint < 32 || (r.codepoint >= 0x7F && r.codepoint < 0xA0) || rune_width(r.codepoint) > 0) {
            ++count;
        }
        i += static_cast<std::size_t>(r.length);
    }
    return count;
}

std::string truncate_width(std::string_view s, int width, bool ellipsis) {
    if (width <= 0) return std::string();
    if (text_width(s) <= width) return std::string(s);

    const int target = ellipsis ? width - 1 : width;
    std::string out;
    out.reserve(s.size());
    int used = 0;
    std::size_t i = 0;
    while (i < s.size() && used < target) {
        const Rune r = decode_rune(s, i);
        const int w = rune_width(r.codepoint);
        if (used + w > target) break;
        if (r.codepoint < 32 || (r.codepoint >= 0x7F && r.codepoint < 0xA0)) {
            out.append("\xC2\xB7"); // U+00B7
        } else {
            out.append(s.substr(i, static_cast<std::size_t>(r.length)));
        }
        used += w;
        i += static_cast<std::size_t>(r.length);
    }
    if (ellipsis) out.append("\xE2\x80\xA6"); // U+2026
    return out;
}

void append_safe(std::string& out, std::string_view s, int max_width) {
    if (max_width <= 0) return;
    int used = 0;
    std::size_t i = 0;
    while (i < s.size() && used < max_width) {
        const Rune r = decode_rune(s, i);
        const int w = rune_width(r.codepoint);
        if (w == 0 && r.codepoint != 0) {
            // Combining mark or zero-width: keep it attached to the previous glyph.
            if (used > 0 && max_width > 0) out.append(s.substr(i, static_cast<std::size_t>(r.length)));
        } else if (w == 0) {
            out.append("\xC2\xB7");
            used += 1;
        } else if (used + w <= max_width) {
            out.append(s.substr(i, static_cast<std::size_t>(r.length)));
            used += w;
        } else {
            break;
        }
        i += static_cast<std::size_t>(r.length);
    }
}

std::string sanitize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const Rune r = decode_rune(s, i);
        if (!r.valid || r.codepoint < 32 || (r.codepoint >= 0x7F && r.codepoint < 0xA0) ||
            r.codepoint == 0x2028 || r.codepoint == 0x2029) {
            out.append("\xC2\xB7");
        } else {
            out.append(s.substr(i, static_cast<std::size_t>(r.length)));
        }
        i += static_cast<std::size_t>(r.length);
    }
    return out;
}

std::string ellipsize_path(std::string_view path, int width) {
    if (width <= 0) return std::string();
    if (text_width(path) <= width) return std::string(path);
    if (width <= 3) return std::string("...").substr(0, static_cast<std::size_t>(width));

    // Prefer keeping the file name, then the parent directory.
    std::size_t slash = path.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? path : path.substr(slash + 1));
    if (text_width(name) + 4 >= width) {
        return "..." + truncate_width(name, width - 3, false);
    }
    std::string parent;
    if (slash != std::string_view::npos && slash > 0) {
        const std::size_t prev = path.find_last_of("/\\", slash - 1);
        parent = std::string(path.substr(prev == std::string_view::npos ? 0 : prev + 1,
                                        slash - (prev == std::string_view::npos ? 0 : prev + 1)));
    }
    std::string tail = parent.empty() ? name : parent + "/" + name;
    const int budget = width - 3;
    const int tail_width = text_width(tail);
    if (tail_width <= budget) {
        return "..." + tail;
    }
    return "..." + truncate_width(tail, budget, false);
}

// ── Misc ────────────────────────────────────────────────────────────────────
std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::size_t clamp_size(std::size_t v, std::size_t lo, std::size_t hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ── File system ──────────────────────────────────────────────────────────────
#if defined(TESSERA_WINDOWS)

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) fail("path is not valid UTF-8: " + text);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::FILE* fopen_utf8(const std::string& path, const char* mode) {
    const std::wstring wide_path = utf8_to_wide(path);
    const std::wstring wide_mode = utf8_to_wide(mode);
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
}

bool file_exists(const std::string& path) {
    const std::wstring wide = utf8_to_wide(path);
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return false;
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void configure_console() {
    // Input as well as output: pasted or typed non-ASCII text must survive.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    const DWORD vt = ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    for (const DWORD which : {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
        const HANDLE handle = GetStdHandle(which);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) continue;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode)) continue; // redirected to a pipe: nothing to do
        SetConsoleMode(handle, mode | vt);
    }
}

#else

void configure_console() {}

std::FILE* fopen_utf8(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

bool file_exists(const std::string& path) {
    std::FILE* probe = std::fopen(path.c_str(), "rb");
    if (probe == nullptr) return false;
    std::fclose(probe);
    return true;
}

#endif

} // namespace tess
