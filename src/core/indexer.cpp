// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · indexer.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/indexer.hpp"

#include "common/util.hpp"
#include "core/simd.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

namespace tess {
namespace {

constexpr std::uint64_t kMinParallelBytes = 4ull << 20; // 4 MiB per thread minimum
constexpr std::uint64_t kMaxQuotedField = 1ull << 20;   // repair heuristic

/// Appends record starts to `out` for [begin, end), starting in `in_quote`.
void scan_range(const char* data, std::uint64_t size, std::uint64_t begin, std::uint64_t end,
                char quote, bool in_quote, std::vector<std::uint64_t>& out) {
    if (quote == 0) {
        const char* p = data + begin;
        const char* const limit = data + end;
        while (p < limit) {
            const char* hit = swar::find_byte(p, limit, '\n');
            if (hit == limit) break;
            const std::uint64_t next = static_cast<std::uint64_t>(hit - data) + 1;
            if (next < size) out.push_back(next);
            p = hit + 1;
        }
        return;
    }

    std::uint64_t i = begin;
    const char* const limit = data + end;
    while (i < end) {
        const char* p = swar::find_either(data + i, limit, '\n', quote);
        if (p == limit) break;
        const std::uint64_t pos = static_cast<std::uint64_t>(p - data);
        if (*p == quote) {
            in_quote = !in_quote;
        } else if (!in_quote) {
            if (pos + 1 < size) out.push_back(pos + 1);
        }
        i = pos + 1;
    }
}

/// Drops offsets whose record contains nothing but its own line terminator.
///
/// Real exports have stray blank lines — a trailing `\n\n`, a spacer row, a
/// double newline from a broken writer. Counting those as data rows inflates
/// the row count and fills the grid with empty records, so they are skipped at
/// the index level, where every consumer gets the benefit. This matches what
/// every mainstream CSV reader does, and it applies to a single-column file
/// too, where an empty line is indistinguishable from an empty value.
void drop_blank_records(const char* data, std::uint64_t size, std::vector<std::uint64_t>& offsets) {
    if (offsets.empty()) return;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const std::uint64_t begin = offsets[i];
        const std::uint64_t end = (i + 1 < offsets.size()) ? offsets[i + 1] : size;
        std::uint64_t scanned = begin;
        while (scanned < end && (data[scanned] == '\r' || data[scanned] == '\n')) ++scanned;
        if (scanned < end) offsets[kept++] = begin; // a record with any content at all
    }
    offsets.resize(kept);
}

} // namespace

unsigned default_thread_count(unsigned requested) noexcept {
    if (requested > 0) return std::min(requested, 64u);
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 2;
    return std::min(hw, 16u);
}

void build_row_index_sequential(const char* data, std::uint64_t size, std::uint64_t begin, char quote,
                                std::vector<std::uint64_t>& out, bool repair) {
    out.clear();
    if (size <= begin) return;
    out.push_back(begin);

    if (quote == 0) {
        const char* p = data + begin;
        const char* const end = data + size;
        while (p < end) {
            const char* hit = swar::find_byte(p, end, '\n');
            if (hit == end) break;
            const std::uint64_t next = static_cast<std::uint64_t>(hit - data) + 1;
            if (next < size) out.push_back(next);
            p = hit + 1;
        }
        return;
    }

    bool in_quotes = false;
    std::uint64_t quote_run_start = 0;
    std::uint64_t i = begin;
    const char* const end = data + size;
    while (i < size) {
        const char* p = swar::find_either(data + i, end, '\n', quote);
        if (p == end) break;
        const std::uint64_t pos = static_cast<std::uint64_t>(p - data);
        if (*p == quote) {
            in_quotes = !in_quotes;
            if (in_quotes) {
                quote_run_start = pos;
            }
            i = pos + 1;
            continue;
        }
        if (in_quotes) {
            // A runaway quote would swallow the rest of the file; recover.
            if (repair && pos - quote_run_start > kMaxQuotedField) {
                in_quotes = false;
                if (pos + 1 < size) out.push_back(pos + 1);
            }
        } else if (pos + 1 < size) {
            out.push_back(pos + 1);
        }
        i = pos + 1;
    }
}

void build_row_index(const char* data, std::uint64_t size, std::uint64_t begin, char quote,
                     unsigned threads, CancelToken* cancel, const ProgressFn& progress,
                     std::vector<std::uint64_t>& out, IndexStats& stats) {
    out.clear();
    stats = IndexStats{};
    if (size <= begin) return;

    const double started = now_seconds();
    const unsigned workers = default_thread_count(threads);
    const std::uint64_t span = size - begin;
    const bool parallel = workers > 1 && span >= kMinParallelBytes;

    // Total quote count decides whether the parity trick is trustworthy.
    if (quote != 0) {
        const std::uint64_t quotes = swar::count_byte(data + begin, data + size, quote);
        if ((quotes & 1u) != 0) {
            // Unbalanced quotes: fall back to the repairing sequential scan.
            build_row_index_sequential(data, size, begin, quote, out, true);
            drop_blank_records(data, size, out);
            stats.repaired_quotes = true;
            stats.rows = out.size();
            stats.seconds = now_seconds() - started;
            if (progress) progress(1.0, "indexing (repaired quoting)");
            return;
        }
    }

    if (!parallel) {
        build_row_index_sequential(data, size, begin, quote, out, false);
        drop_blank_records(data, size, out);
    drop_blank_records(data, size, out);
    stats.rows = out.size();
    stats.seconds = now_seconds() - started;
    if (progress) progress(1.0, "indexing");
    return;
}

    // ── Split into chunks aligned to byte boundaries (not records). ───────────
    const std::uint64_t chunk_count_u64 = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(workers) * 4ull, std::max<std::uint64_t>(1, span / kMinParallelBytes));
    const auto chunk_count = static_cast<std::size_t>(std::max<std::uint64_t>(1, chunk_count_u64));
    std::vector<std::uint64_t> bounds(chunk_count + 1);
    bounds[0] = begin;
    for (std::size_t i = 1; i < chunk_count; ++i) {
        bounds[i] = begin + (span * static_cast<std::uint64_t>(i)) / chunk_count;
    }
    bounds[chunk_count] = size;

    // ── Pass 1: quotes per chunk (also warms the page cache). ────────────────
    std::vector<std::uint64_t> quote_counts(chunk_count, 0);
    {
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::vector<std::thread> pool;
        pool.reserve(std::min<std::size_t>(chunk_count, workers));
        for (unsigned t = 0; t < workers && t < chunk_count; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= chunk_count) return;
                    if (cancel != nullptr) cancel->throw_if_canceled();
                    quote_counts[idx] = quote == 0 ? 0 : swar::count_byte(data + bounds[idx], data + bounds[idx + 1], quote);
                    const std::size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (progress) progress(0.5 * static_cast<double>(finished) / static_cast<double>(chunk_count), "indexing");
                }
            });
        }
        for (auto& th : pool) {
            if (th.joinable()) th.join();
        }
    }
    if (cancel != nullptr) cancel->throw_if_canceled();

    std::vector<bool> starts_in_quote(chunk_count, false);
    std::uint64_t prefix = 0;
    for (std::size_t i = 0; i < chunk_count; ++i) {
        starts_in_quote[i] = (prefix & 1u) != 0;
        prefix += quote_counts[i];
    }

    // ── Pass 2: record starts per chunk, merged in order. ────────────────────
    std::vector<std::vector<std::uint64_t>> per_chunk(chunk_count);
    {
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::vector<std::thread> pool;
        pool.reserve(std::min<std::size_t>(chunk_count, workers));
        for (unsigned t = 0; t < workers && t < chunk_count; ++t) {
            pool.emplace_back([&] {
                for (;;) {
                    const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= chunk_count) return;
                    if (cancel != nullptr) cancel->throw_if_canceled();
                    scan_range(data, size, bounds[idx], bounds[idx + 1], quote, starts_in_quote[idx],
                               per_chunk[idx]);
                    const std::size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (progress) {
                        progress(0.5 + 0.5 * static_cast<double>(finished) / static_cast<double>(chunk_count),
                                 "indexing");
                    }
                }
            });
        }
        for (auto& th : pool) {
            if (th.joinable()) th.join();
        }
    }
    if (cancel != nullptr) cancel->throw_if_canceled();

    std::size_t total = 1; // the first record always starts at `begin`
    for (const auto& chunk : per_chunk) total += chunk.size();
    out.clear();
    out.reserve(total);
    out.push_back(begin);
    for (const auto& chunk : per_chunk) out.insert(out.end(), chunk.begin(), chunk.end());

    // Defensive: chunks are disjoint and increasing, but a corrupted parity
    // calculation must never produce an unusable index.
    if (!std::is_sorted(out.begin(), out.end())) {
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    stats.rows = out.size();
    stats.seconds = now_seconds() - started;
    if (progress) progress(1.0, "indexing");
}

} // namespace tess
