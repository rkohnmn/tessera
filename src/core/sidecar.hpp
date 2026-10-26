// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · sidecar.hpp
//  Optional `.tessera.idx` cache so the tenth open of a 60 GB export is instant
//  instead of merely fast. Offsets are delta + LEB128 encoded: a million-row
//  file costs roughly 1.5 MB instead of 8 MB, and validation is a magic number
//  plus size/mtime/header signature so a stale cache is never trusted.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tess {

[[nodiscard]] std::string index_cache_path(const std::string& data_path);

struct IndexCacheInfo {
    std::uint64_t row_count = 0;
    std::uint64_t bytes = 0;
};

/// Returns the cached offsets when the sidecar exists and matches the file.
[[nodiscard]] std::optional<std::vector<std::uint64_t>> load_index_cache(const std::string& data_path,
                                                                        std::uint64_t data_size,
                                                                        std::int64_t mtime,
                                                                        std::uint64_t header_signature,
                                                                        IndexCacheInfo* info = nullptr);

/// Best-effort write; returns false (never throws) when the directory is read-only.
[[nodiscard]] bool save_index_cache(const std::string& data_path, std::uint64_t data_size,
                                    std::int64_t mtime, std::uint64_t header_signature,
                                    const std::vector<std::uint64_t>& offsets, char delimiter, char quote,
                                    bool has_header);

/// 64-bit FNV-1a, used for header signatures and sidecar integrity.
[[nodiscard]] std::uint64_t fnv1a(std::string_view data, std::uint64_t seed = 1469598103934665603ull) noexcept;

} // namespace tess
