// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · profile.cpp
// ─────────────────────────────────────────────────────────────────────────────
#include "core/profile.hpp"

#include "common/util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace tess {
namespace {

constexpr std::size_t kReservoir = 65536;
constexpr std::size_t kTopValueTrack = 20000;
constexpr std::size_t kExactDistinctLimit = 2048;
constexpr std::size_t kHistogramBuckets = 24;
constexpr std::size_t kHashBytes = 32;

std::uint64_t hash_cell(std::string_view value) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    const std::size_t limit = std::min<std::size_t>(value.size(), kHashBytes);
    for (std::size_t i = 0; i < limit; ++i) {
        h ^= static_cast<unsigned char>(value[i]);
        h *= 1099511628211ull;
    }
    h ^= static_cast<std::uint64_t>(value.size()) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

double quantile(const std::vector<double>& sorted, double q) {
    if (sorted.empty()) return 0.0;
    const double position = q * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(std::max(0.0, position));
    if (index + 1 >= sorted.size()) return sorted.back();
    const double fraction = position - static_cast<double>(index);
    return sorted[index] * (1.0 - fraction) + sorted[index + 1] * fraction;
}

} // namespace

// ── HyperLogLog ─────────────────────────────────────────────────────────────
HyperLogLog::HyperLogLog() : registers_(kRegisters, 0) {}

void HyperLogLog::add(std::string_view value) noexcept {
    const std::uint64_t hash = hash_cell(value);
    const std::uint32_t index = static_cast<std::uint32_t>(hash >> (64 - kPrecision));
    const std::uint64_t remaining = (hash << kPrecision) | (1ull << (kPrecision - 1));
    const auto rank = static_cast<std::uint8_t>(std::countl_zero(remaining) + 1);
    if (rank > registers_[index]) registers_[index] = rank;
}

void HyperLogLog::merge(const HyperLogLog& other) noexcept {
    for (std::size_t i = 0; i < kRegisters; ++i) {
        registers_[i] = std::max(registers_[i], other.registers_[i]);
    }
}

std::uint64_t HyperLogLog::estimate() const noexcept {
    const double m = static_cast<double>(kRegisters);
    double sum = 0.0;
    std::size_t zeros = 0;
    for (const std::uint8_t reg : registers_) {
        sum += std::pow(2.0, -static_cast<double>(reg));
        if (reg == 0) ++zeros;
    }
    const double alpha = 0.7213 / (1.0 + 1.079 / m);
    double estimate_value = alpha * m * m / sum;
    if (estimate_value <= 2.5 * m && zeros > 0) {
        estimate_value = m * std::log(m / static_cast<double>(zeros));
    }
    return static_cast<std::uint64_t>(estimate_value + 0.5);
}

namespace {

struct Accumulator {
    std::uint64_t total = 0;
    std::uint64_t nulls = 0;
    std::uint64_t numeric_count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double m2 = 0.0;
    std::int64_t time_min = 0;
    std::int64_t time_max = 0;
    bool has_time = false;
    std::uint64_t min_length = 0;
    std::uint64_t max_length = 0;
    std::uint64_t length_sum = 0;
    std::vector<double> sample;
    std::vector<double> time_sample;
    HyperLogLog hll;
    std::unordered_set<std::string> exact_distinct;
    bool exact_overflow = false;
    std::unordered_map<std::string, std::uint64_t> top;
    bool top_overflow = false;
    std::uint64_t seen = 0;
};

void add_distinct(Accumulator& acc, std::string_view text) {
    if (!acc.exact_overflow) {
        if (acc.exact_distinct.size() < kExactDistinctLimit) {
            acc.exact_distinct.emplace(text);
            return;
        }
        for (const auto& value : acc.exact_distinct) acc.hll.add(value);
        acc.exact_distinct.clear();
        acc.exact_distinct.rehash(0);
        acc.exact_overflow = true;
    }
    acc.hll.add(text);
}

void add_numeric_sample(Accumulator& acc, double value) {
    if (acc.sample.size() < kReservoir) {
        acc.sample.push_back(value);
        return;
    }
    // Deterministic stride thinning keeps memory bounded and unbiased enough
    // for an at-a-glance distribution.
    ++acc.seen;
    if ((acc.seen & 3u) == 0) acc.sample[acc.seen % kReservoir] = value;
}

void add_top_value(Accumulator& acc, const std::string& text) {
    if (acc.top_overflow) return;
    auto it = acc.top.find(text);
    if (it != acc.top.end()) {
        ++it->second;
        return;
    }
    if (acc.top.size() >= kTopValueTrack) {
        acc.top_overflow = true;
        return;
    }
    acc.top.emplace(text, 1);
}

} // namespace

ColumnProfile profile_column(const Dataset& dataset, std::uint32_t column, const ProfileOptions& options) {
    const double started = now_seconds();
    ColumnProfile profile;
    profile.name = std::string(dataset.column_name(column));
    profile.type = dataset.column_type(column);
    if (column >= dataset.column_count()) return profile;

    Accumulator acc;
    const std::uint64_t total_rows = options.rows != nullptr ? options.rows->size() : dataset.row_count();
    std::uint64_t stride = 1;
    if (options.sample_limit > 0 && total_rows > options.sample_limit) {
        stride = std::max<std::uint64_t>(1, total_rows / options.sample_limit);
    }

    const char delimiter = dataset.dialect().delimiter;
    const char quote = dataset.dialect().quote;
    std::uint64_t processed = 0;
    for (std::uint64_t i = 0; i < total_rows; i += stride) {
        if ((processed & 262143u) == 0) {
            if (options.cancel != nullptr) options.cancel->throw_if_canceled();
            if (options.progress && total_rows > 0) {
                options.progress(static_cast<double>(i) / static_cast<double>(total_rows), "profiling");
            }
        }
        ++processed;
        const std::uint64_t row = options.rows != nullptr ? (*options.rows)[static_cast<std::size_t>(i)] : i;
        const std::string_view row_text = dataset.row_text(row);
        FieldSpan span;
        if (!field_at(row_text, delimiter, quote, column, span)) {
            ++acc.total;
            ++acc.nulls;
            continue;
        }
        const std::string_view raw = row_text.substr(span.offset, span.length);
        ++acc.total;
        if (raw.empty()) {
            ++acc.nulls;
            continue;
        }
        add_distinct(acc, raw);

        const std::string_view text = trim(raw);
        acc.min_length = acc.min_length == 0 ? text.size() : std::min<std::uint64_t>(acc.min_length, text.size());
        acc.max_length = std::max<std::uint64_t>(acc.max_length, text.size());
        acc.length_sum += text.size();

        if (is_numeric(profile.type) || profile.type == ColumnType::Boolean) {
            double value = 0.0;
            bool ok = false;
            if (profile.type == ColumnType::Boolean) {
                bool b = false;
                if (parse_bool(text, b)) {
                    value = b ? 1.0 : 0.0;
                    ok = true;
                }
            } else {
                std::int64_t iv = 0;
                if (parse_int(text, iv)) {
                    value = static_cast<double>(iv);
                    ok = true;
                } else if (parse_double(text, value)) {
                    ok = true;
                }
            }
            if (ok) {
                if (acc.numeric_count == 0) {
                    acc.min = acc.max = value;
                } else {
                    acc.min = std::min(acc.min, value);
                    acc.max = std::max(acc.max, value);
                }
                ++acc.numeric_count;
                const double delta = value - acc.mean;
                acc.mean += delta / static_cast<double>(acc.numeric_count);
                acc.m2 += delta * (value - acc.mean);
                add_numeric_sample(acc, value);
            }
        }

        if (profile.type == ColumnType::Timestamp) {
            if (const auto stamp = parse_timestamp(text)) {
                if (!acc.has_time) {
                    acc.time_min = acc.time_max = *stamp;
                    acc.has_time = true;
                } else {
                    acc.time_min = std::min(acc.time_min, *stamp);
                    acc.time_max = std::max(acc.time_max, *stamp);
                }
                acc.time_sample.push_back(static_cast<double>(*stamp));
            }
        }

        if (profile.type == ColumnType::Text || profile.type == ColumnType::Unknown) {
            add_top_value(acc, std::string(text));
        }
    }

    profile.total = acc.total;
    profile.nulls = acc.nulls;
    profile.distinct_estimate = acc.exact_overflow ? acc.hll.estimate() : acc.exact_distinct.size();
    profile.distinct_is_estimate = acc.exact_overflow;
    profile.numeric = acc.numeric_count > 0;
    profile.temporal = acc.has_time;

    if (profile.numeric) {
        profile.min = acc.min;
        profile.max = acc.max;
        profile.mean = acc.mean;
        profile.stddev = acc.numeric_count > 1
                             ? std::sqrt(acc.m2 / static_cast<double>(acc.numeric_count - 1))
                             : 0.0;
        std::vector<double> sorted = acc.sample;
        std::sort(sorted.begin(), sorted.end());
        profile.p25 = quantile(sorted, 0.25);
        profile.median = quantile(sorted, 0.5);
        profile.p75 = quantile(sorted, 0.75);
        profile.p99 = quantile(sorted, 0.99);

        profile.histogram.resize(kHistogramBuckets);
        const double span = profile.max - profile.min;
        for (auto& bucket : profile.histogram) bucket = Bucket{};
        if (span > 0.0) {
            for (std::size_t b = 0; b < kHistogramBuckets; ++b) {
                profile.histogram[b].low = profile.min + span * static_cast<double>(b) / kHistogramBuckets;
                profile.histogram[b].high = profile.min + span * static_cast<double>(b + 1) / kHistogramBuckets;
            }
            for (const double value : sorted) {
                std::size_t index = static_cast<std::size_t>((value - profile.min) / span * kHistogramBuckets);
                index = std::min(index, kHistogramBuckets - 1);
                ++profile.histogram[index].count;
            }
        } else if (!sorted.empty()) {
            profile.histogram[0] = Bucket{profile.min, profile.max, static_cast<std::uint64_t>(sorted.size())};
        }
    }

    if (profile.temporal) {
        profile.time_min = acc.time_min;
        profile.time_max = acc.time_max;
        std::sort(acc.time_sample.begin(), acc.time_sample.end());
        profile.histogram.resize(kHistogramBuckets);
        const double span = static_cast<double>(acc.time_max - acc.time_min);
        if (span > 0.0) {
            for (std::size_t b = 0; b < kHistogramBuckets; ++b) {
                profile.histogram[b].low = static_cast<double>(acc.time_min) + span * static_cast<double>(b) / kHistogramBuckets;
                profile.histogram[b].high = static_cast<double>(acc.time_min) + span * static_cast<double>(b + 1) / kHistogramBuckets;
            }
            for (const double value : acc.time_sample) {
                std::size_t index = static_cast<std::size_t>((value - static_cast<double>(acc.time_min)) / span *
                                                             kHistogramBuckets);
                index = std::min(index, kHistogramBuckets - 1);
                ++profile.histogram[index].count;
            }
        }
    }

    if (!acc.top.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> values(acc.top.begin(), acc.top.end());
        std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });
        if (values.size() > 6) values.resize(6);
        profile.top_values = std::move(values);
        profile.top_values_truncated = acc.top_overflow;
    }

    profile.min_length = acc.min_length;
    profile.max_length = acc.max_length;
    profile.mean_length = acc.total > acc.nulls
                              ? static_cast<double>(acc.length_sum) / static_cast<double>(acc.total - acc.nulls)
                              : 0.0;
    profile.seconds = now_seconds() - started;
    return profile;
}

std::vector<ColumnProfile> profile_columns(const Dataset& dataset, const std::vector<std::uint32_t>& columns,
                                           const ProfileOptions& options) {
    std::vector<std::uint32_t> targets = columns;
    if (targets.empty()) {
        for (std::uint32_t c = 0; c < dataset.column_count(); ++c) targets.push_back(c);
    }
    std::vector<ColumnProfile> profiles;
    profiles.reserve(targets.size());
    for (const std::uint32_t column : targets) {
        profiles.push_back(profile_column(dataset, column, options));
    }
    return profiles;
}

} // namespace tess
