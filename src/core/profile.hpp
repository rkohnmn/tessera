// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · profile.hpp
//  Column statistics for people who need to know whether a column is usable
//  before they trust it: fill rate, min/max, quantiles, distinct estimate and a
//  distribution histogram. One streaming pass, bounded memory.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "common/error.hpp"
#include "core/dataset.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tess {

/// Exact-count HyperLogLog (2^14 registers, ~0.8% error at 10M distinct).
class HyperLogLog {
public:
    HyperLogLog();
    void add(std::string_view value) noexcept;
    [[nodiscard]] std::uint64_t estimate() const noexcept;
    void merge(const HyperLogLog& other) noexcept;

private:
    static constexpr unsigned kPrecision = 14;
    static constexpr std::size_t kRegisters = std::size_t{1} << kPrecision;
    std::vector<std::uint8_t> registers_;
};

struct Bucket {
    double low = 0.0;
    double high = 0.0;
    std::uint64_t count = 0;
};

struct ColumnProfile {
    std::string name;
    ColumnType type = ColumnType::Text;
    std::uint64_t total = 0;
    std::uint64_t nulls = 0;
    std::uint64_t distinct_estimate = 0;
    bool distinct_is_estimate = true;

    // Numeric summary.
    bool numeric = false;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double stddev = 0.0;
    double p25 = 0.0;
    double median = 0.0;
    double p75 = 0.0;
    double p99 = 0.0;

    // Timestamp summary (Unix seconds).
    bool temporal = false;
    std::int64_t time_min = 0;
    std::int64_t time_max = 0;

    // Text summary.
    std::uint64_t min_length = 0;
    std::uint64_t max_length = 0;
    double mean_length = 0.0;

    std::vector<Bucket> histogram;
    std::vector<std::pair<std::string, std::uint64_t>> top_values;
    bool top_values_truncated = false;
    double seconds = 0.0;
};

struct ProfileOptions {
    /// Restrict to a subset of rows (filtered view). Empty = every row.
    const std::vector<std::uint64_t>* rows = nullptr;
    CancelToken* cancel = nullptr;
    ProgressFn progress;
    /// 0 = profile every row; otherwise stride-sample at most this many rows.
    std::uint64_t sample_limit = 0;
};

[[nodiscard]] ColumnProfile profile_column(const Dataset& dataset, std::uint32_t column,
                                           const ProfileOptions& options);

[[nodiscard]] std::vector<ColumnProfile> profile_columns(const Dataset& dataset,
                                                         const std::vector<std::uint32_t>& columns,
                                                         const ProfileOptions& options);

} // namespace tess
