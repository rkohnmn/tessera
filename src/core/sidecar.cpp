// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · sidecar.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/sidecar.hpp"

#include "common/error.hpp"
#include "core/source.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace tess {
namespace {

// Bump both when the set or meaning of the offsets changes: v3 stores offsets
// with blank records already filtered out, so a v2 cache would reintroduce the
// empty rows that v3 removes. v4 widened the checksum to cover the whole record,
// not just the payload, so damage to a field that only describes the file is
// detected instead of going unnoticed. Validation rejects anything it did not
// write, and an older cache is simply rebuilt.
constexpr std::array<char, 8> kMagic = {'T', 'E', 'S', 'S', 'I', 'D', 'X', '4'};
constexpr std::uint32_t kVersion = 4;

// magic(8) version(4) data_size(8) mtime(8) header_sig(8) — up to here every
// byte is compared against the live file, so those need no checksum. Everything
// from the descriptor fields onwards is only covered by the checksum.
constexpr std::size_t kChecksumFrom = 8 + 4 + 8 + 8 + 8;

/// Hard ceiling on how much of a `.tessera.idx` we will load. The cache is a
/// pure optimisation: if it is larger than this, something has corrupted it and
/// rebuilding the index is always the better answer.
constexpr std::uint64_t kMaxCacheBytes = 512ull * 1024ull * 1024ull;

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

std::uint32_t get_u32(const char* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

std::uint64_t get_u64(const char* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
    return v;
}

void put_varint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

bool get_varint(const char*& p, const char* end, std::uint64_t& out) {
    std::uint64_t result = 0;
    int shift = 0;
    while (p < end && shift <= 63) {
        const auto byte = static_cast<unsigned char>(*p++);
        // A varint long enough to shift into the sign bit is malformed: the
        // high bits would be silently discarded, which is exactly the kind of
        // corruption a cache must never paper over.
        if (shift == 63 && (byte & 0x7F) > 1) return false;
        result |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

} // namespace

std::uint64_t fnv1a(std::string_view data, std::uint64_t seed) noexcept {
    std::uint64_t hash = seed;
    for (const char ch : data) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string index_cache_path(const std::string& data_path) {
    return data_path + ".tessera.idx";
}

std::optional<std::vector<std::uint64_t>> load_index_cache(const std::string& data_path,
                                                           std::uint64_t data_size,
                                                           std::int64_t mtime,
                                                           std::uint64_t header_signature,
                                                           IndexCacheInfo* info) {
    const std::string path = index_cache_path(data_path);
    // A sidecar is never trusted, only verified: bound the read before a single
    // byte is allocated so a corrupt or hostile cache cannot exhaust memory.
    const std::optional<std::string> loaded = read_file_limited(path, kMaxCacheBytes);
    if (!loaded.has_value()) return std::nullopt;
    const std::string& bytes = *loaded;
    // magic(8) version(4) data_size(8) mtime(8) header_sig(8) delimiter(1)
    // quote(1) has_header(1) pad(5) row_count(8) | payload... | checksum(8)
    // The checksum trails the payload so it covers one contiguous prefix that
    // includes the row count, the descriptor fields and every offset.
    constexpr std::size_t kHeaderSize = 8 + 4 + 8 + 8 + 8 + 1 + 1 + 1 + 5 + 8;
    constexpr std::size_t kFooterSize = 8;
    if (bytes.size() < kHeaderSize + kFooterSize) return std::nullopt;
    const char* p = bytes.data();
    if (std::memcmp(p, kMagic.data(), kMagic.size()) != 0) return std::nullopt;
    p += kMagic.size();
    if (get_u32(p) != kVersion) return std::nullopt;
    p += 4;
    const std::uint64_t stored_size = get_u64(p);
    p += 8;
    const std::uint64_t stored_mtime = get_u64(p);
    p += 8;
    const std::uint64_t stored_sig = get_u64(p);
    p += 8;
    ++p; // delimiter (informational)
    ++p; // quote
    ++p; // has_header
    p += 5;
    const std::uint64_t row_count = get_u64(p);
    p += 8;
    const char* const payload_end = bytes.data() + bytes.size() - kFooterSize;
    const std::uint64_t checksum = get_u64(payload_end);

    if (stored_size != data_size) return std::nullopt;
    if (stored_size == 0) return std::nullopt;
    if (stored_mtime != static_cast<std::uint64_t>(mtime)) return std::nullopt;
    if (stored_sig != header_signature) return std::nullopt;

    const std::size_t payload_bytes = static_cast<std::size_t>(payload_end - (bytes.data() + kHeaderSize));
    // Checksum over the descriptor fields *and* the payload: the delimiter,
    // quote and header flag are informational, but a cache whose bytes have
    // rotted must not be accepted just because the part we happen to read first
    // still matches. This runs before any allocation is sized from the file.
    if (fnv1a(std::string_view(bytes.data() + kChecksumFrom, static_cast<std::size_t>(payload_end - (bytes.data() + kChecksumFrom)))) != checksum) {
        return std::nullopt;
    }
    // Each record in the source file is at least one byte, and each offset costs
    // at least one payload byte, so a row count above either bound is a lie.
    if (row_count == 0 || row_count > static_cast<std::uint64_t>(payload_bytes)) return std::nullopt;
    if (row_count > data_size) return std::nullopt;

    std::vector<std::uint64_t> offsets;
    // Reserve a modest amount and let the vector grow while decoding, rather than
    // trusting the header's row count. Corrupt input then fails during the parse
    // instead of after a multi-gigabyte allocation.
    offsets.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(row_count, 1u << 16)));
    const char* const end = payload_end;
    std::uint64_t current = 0;
    for (std::uint64_t i = 0; i < row_count; ++i) {
        std::uint64_t delta = 0;
        if (!get_varint(p, end, delta)) return std::nullopt;
        // `data_size - current` first, so a delta crafted near UINT64_MAX cannot
        // wrap the accumulator and produce a plausible-looking offset.
        if (delta >= data_size - current) return std::nullopt;
        current += delta;
        if (i == 0) {
            offsets.push_back(current);
        } else {
            if (current <= offsets.back()) return std::nullopt;
            offsets.push_back(current);
        }
    }
    if (offsets.empty() || offsets.front() >= data_size) return std::nullopt;
    // Trailing bytes mean the payload is not what the header describes.
    if (p != end) return std::nullopt;
    if (info != nullptr) {
        info->row_count = row_count;
        info->bytes = bytes.size();
    }
    return offsets;
}

bool save_index_cache(const std::string& data_path, std::uint64_t data_size, std::int64_t mtime,
                      std::uint64_t header_signature, const std::vector<std::uint64_t>& offsets,
                      char delimiter, char quote, bool has_header) {
    if (offsets.empty()) return false;
    std::string payload;
    payload.reserve(offsets.size() * 2);
    std::uint64_t previous = 0;
    for (const std::uint64_t offset : offsets) {
        put_varint(payload, offset - previous);
        previous = offset;
    }

    std::string out;
    out.reserve(payload.size() + 64);
    out.append(kMagic.data(), kMagic.size());
    put_u32(out, kVersion);
    put_u64(out, data_size);
    put_u64(out, static_cast<std::uint64_t>(mtime));
    put_u64(out, header_signature);
    out.push_back(delimiter);
    out.push_back(quote);
    out.push_back(has_header ? 1 : 0);
    out.append(5, '\0');
    put_u64(out, static_cast<std::uint64_t>(offsets.size()));
    out.append(payload);
    // Trailing, so it signs the same contiguous prefix the reader verifies.
    put_u64(out, fnv1a(std::string_view(out.data() + kChecksumFrom, out.size() - kChecksumFrom)));

    try {
        // Atomic: a reader either sees the previous complete cache or this one.
        write_file_atomic(index_cache_path(data_path), out);
        return true;
    } catch (const Error&) {
        return false;
    }
}

} // namespace tess
