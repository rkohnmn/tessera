// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · indexer.hpp
//
//  Builds the row-offset index: an 8-byte entry per record. That is the whole
//  in-memory cost of opening a file — 80 MB for ten million rows — and it is
//  why scrolling a 40 GB CSV is instant while pandas needs 40 GB of RAM.
//
//  Parallelism trick: CSV quoting is a state machine, so a naive thread-per-
//  chunk scan cannot know whether a chunk begins inside a quoted field.
//  However every well-formed record contains an *even* number of quote
//  characters (unquoted fields have none, quoted fields have two plus a pair
//  for every doubled quote). So the quote parity of the prefix up to any
//  offset tells us the exact state at that offset: two cheap passes, one to
//  count quotes per chunk and one to find record starts, no locking.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "common/error.hpp"

#include <cstdint>
#include <vector>

namespace tess {

struct IndexStats {
    double seconds = 0.0;
    std::uint64_t rows = 0;
    bool repaired_quotes = false;
};

/// Fills `out` with the start offset of every physical record.
/// `begin` skips a byte-order mark. `quote == 0` disables quote handling.
void build_row_index(const char* data, std::uint64_t size, std::uint64_t begin, char quote,
                     unsigned threads, CancelToken* cancel, const ProgressFn& progress,
                     std::vector<std::uint64_t>& out, IndexStats& stats);

/// Single-threaded reference implementation. It additionally repairs
/// unbalanced quoting (a stray `"` would otherwise swallow the whole file) by
/// abandoning quote state after 1 MiB without a closing quote.
void build_row_index_sequential(const char* data, std::uint64_t size, std::uint64_t begin, char quote,
                                std::vector<std::uint64_t>& out, bool repair);

/// Default worker count: hardware concurrency clamped to a sane range.
[[nodiscard]] unsigned default_thread_count(unsigned requested = 0) noexcept;

} // namespace tess
