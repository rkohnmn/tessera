// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · simd.hpp
//  Branch-light byte scanning without intrinsics.
//
//  The row indexer is a search for '"' and '\n' at memory bandwidth. Rather
//  than hand-writing intrinsics for every target we use SWAR ("SIMD within a
//  register"): eight bytes per 64-bit word, one mask computation each, with a
//  scalar tail. This keeps the same code path on x86-64, ARM64 and MSVC, which
//  matters for a tool that ships as one binary per platform.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace tess::swar {

inline constexpr std::uint64_t kLowBits = 0x0101010101010101ull;
inline constexpr std::uint64_t kHighBits = 0x8080808080808080ull;

/// Per-byte "is zero" mask. Bit 7 of the corresponding byte is set on a match.
[[nodiscard]] inline std::uint64_t zero_mask(std::uint64_t word) noexcept {
    return (word - kLowBits) & ~word & kHighBits;
}

[[nodiscard]] inline std::uint64_t load64(const char* p) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] inline std::uint64_t splat(char c) noexcept {
    return kLowBits * static_cast<std::uint64_t>(static_cast<unsigned char>(c));
}

/// First index in [0, 8) whose byte equals `c`, or -1 when the word has none.
[[nodiscard]] inline int first_byte(std::uint64_t mask) noexcept {
    if (mask == 0) return -1;
    return static_cast<int>(std::countr_zero(mask) >> 3);
}

/// Scalar search for a single byte.
[[nodiscard]] inline const char* find_byte(const char* p, const char* end, char c) noexcept {
    const std::uint64_t pattern = splat(c);
    while (end - p >= 8) {
        const std::uint64_t mask = zero_mask(load64(p) ^ pattern);
        if (mask != 0) {
            const int idx = first_byte(mask);
            if (p[idx] == c) return p + idx;
            // Rare false positive after a borrow; resume just past it.
            p += idx + 1;
            continue;
        }
        p += 8;
    }
    for (; p < end; ++p) {
        if (*p == c) return p;
    }
    return end;
}

/// Scalar search for either of two bytes; returns the earliest match.
[[nodiscard]] inline const char* find_either(const char* p, const char* end, char a, char b) noexcept {
    const std::uint64_t pa = splat(a);
    const std::uint64_t pb = splat(b);
    while (end - p >= 8) {
        const std::uint64_t word = load64(p);
        const std::uint64_t mask = zero_mask(word ^ pa) | zero_mask(word ^ pb);
        if (mask != 0) {
            const int idx = first_byte(mask);
            if (p[idx] == a || p[idx] == b) return p + idx;
            p += idx + 1;
            continue;
        }
        p += 8;
    }
    for (; p < end; ++p) {
        if (*p == a || *p == b) return p;
    }
    return end;
}

/// Counts occurrences of `c` in [p, end). Used for the parallel quote-parity
/// pass that lets row-index workers begin mid-file with a known quote state.
[[nodiscard]] inline std::uint64_t count_byte(const char* p, const char* end, char c) noexcept {
    const std::uint64_t pattern = splat(c);
    std::uint64_t count = 0;
    while (end - p >= 8) {
        count += static_cast<std::uint64_t>(std::popcount(zero_mask(load64(p) ^ pattern)));
        p += 8;
    }
    for (; p < end; ++p) {
        count += (*p == c) ? 1u : 0u;
    }
    return count;
}

[[nodiscard]] inline bool contains(const char* p, const char* end, char c) noexcept {
    return find_byte(p, end, c) != end;
}

} // namespace tess::swar
