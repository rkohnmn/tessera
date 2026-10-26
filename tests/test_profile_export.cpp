// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_profile_export.cpp
//  Column statistics and the export writer. Between them these cover the two
//  features people quietly depend on: "is this column trustworthy?" and "did my
//  export actually round-trip byte for byte?".
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "common/util.hpp"
#include "core/dataset.hpp"
#include "core/profile.hpp"
#include "io/writer.hpp"
#include "query/exec.hpp"
#include "query/parser.hpp"

#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace tess;
using testing::temp_path;

namespace {

std::shared_ptr<Dataset> numbers() {
    static const std::shared_ptr<Dataset> dataset = Dataset::parse(
        "id,value,label,when\n"
        "1,10,alpha,2024-01-01\n"
        "2,20,beta,2024-01-02\n"
        "3,30,alpha,2024-01-03\n"
        "4,,beta,\n"
        "5,50,alpha,2024-01-05\n"
        "6,60,gamma,2024-01-06\n",
        "numbers.csv");
    return dataset;
}

std::string read_back(const std::string& path) { return read_file(path); }

} // namespace

TEST(profile_numeric_summary_is_exact) {
    const ColumnProfile p = profile_column(*numbers(), 1, {});
    CHECK(p.numeric);
    CHECK_EQ(p.total, std::uint64_t{6});
    CHECK_EQ(p.nulls, std::uint64_t{1});
    CHECK_NEAR(p.min, 10.0, 1e-9);
    CHECK_NEAR(p.max, 60.0, 1e-9);
    CHECK_NEAR(p.mean, 34.0, 1e-9);
    CHECK_NEAR(p.median, 30.0, 1e-9);
    CHECK_NEAR(p.stddev, 20.7364, 0.01); // sample standard deviation
    CHECK(p.histogram.size() >= 4);
    std::uint64_t bucket_total = 0;
    for (const auto& bucket : p.histogram) bucket_total += bucket.count;
    CHECK_EQ(bucket_total, std::uint64_t{5});
}

TEST(profile_text_column_distinct_and_top_values) {
    const ColumnProfile p = profile_column(*numbers(), 2, {});
    CHECK(!p.numeric);
    CHECK_EQ(p.nulls, std::uint64_t{0});
    CHECK_EQ(p.distinct_estimate, std::uint64_t{3});
    CHECK_EQ(p.top_values.size(), std::size_t{3});
    CHECK_EQ(p.top_values[0].first, std::string("alpha"));
    CHECK_EQ(p.top_values[0].second, std::uint64_t{3});
    CHECK_EQ(p.min_length, std::uint64_t{4});
    CHECK_EQ(p.max_length, std::uint64_t{5});
}

TEST(profile_timestamp_bounds) {
    const ColumnProfile p = profile_column(*numbers(), 3, {});
    CHECK(p.temporal);
    CHECK_EQ(p.nulls, std::uint64_t{1});
    CHECK(format_timestamp(p.time_min, true).find("2024-01-01") != std::string::npos);
    CHECK(format_timestamp(p.time_max, true).find("2024-01-06") != std::string::npos);
}

TEST(profile_respects_row_subset) {
    std::vector<std::uint64_t> rows{0, 1};
    ProfileOptions options;
    options.rows = &rows;
    const ColumnProfile p = profile_column(*numbers(), 1, options);
    CHECK_EQ(p.total, std::uint64_t{2});
    CHECK_NEAR(p.mean, 15.0, 1e-9);
}

TEST(profile_sampling_stays_bounded) {
    ProfileOptions options;
    options.sample_limit = 3;
    const ColumnProfile p = profile_column(*numbers(), 1, options);
    CHECK(p.total <= 3);
}

TEST(hyperloglog_is_close_on_many_values) {
    HyperLogLog sketch;
    for (int i = 0; i < 10000; ++i) {
        const std::string value = "id-" + std::to_string(i);
        sketch.add(value);
    }
    const std::uint64_t estimate = sketch.estimate();
    const double error = std::fabs(static_cast<double>(estimate) - 10000.0) / 10000.0;
    CHECK(error < 0.05);
}

TEST(export_preserves_bytes_and_quoting) {
    const std::string path = temp_path("test_export.csv");
    const std::string original =
        "id,note\n"
        "1,plain\n"
        "2,\"has,comma\"\n"
        "3,\"say \"\"hi\"\"\"\n"
        "4,\"two\nlines\"\n";
    write_file(temp_path("test_export_src.csv"), original);
    OpenOptions options;
    options.use_sidecar = false;
    auto dataset = Dataset::open(temp_path("test_export_src.csv"), options);
    const auto stats = io::export_rows(*dataset, nullptr, {}, path, io::OutputFormat::Csv, {});
    CHECK_EQ(stats.rows, std::uint64_t{4});
    CHECK_EQ(read_back(path), original);
    std::remove(temp_path("test_export.csv").c_str());
    std::remove(temp_path("test_export_src.csv").c_str());
}

TEST(export_tsv_rejoins_fields_with_tabs) {
    const std::string path = temp_path("test_export.tsv");
    const std::string original = "id,note\n1,alpha\n2,\"a,b\"\n";
    write_file(temp_path("test_export_src2.csv"), original);
    OpenOptions options;
    options.use_sidecar = false;
    auto dataset = Dataset::open(temp_path("test_export_src2.csv"), options);
    const auto stats = io::export_rows(*dataset, nullptr, {}, path, io::OutputFormat::Tsv, {});
    CHECK_EQ(stats.rows, std::uint64_t{2});
    const std::string tsv = read_back(path);
    CHECK(tsv.find("id\tnote") != std::string::npos);
    CHECK(tsv.find("2\t\"a,b\"") != std::string::npos); // quoting is preserved
    std::remove(temp_path("test_export.tsv").c_str());
    std::remove(temp_path("test_export_src2.csv").c_str());
}

TEST(export_jsonl_emits_one_object_per_row) {
    const std::string path = temp_path("test_export.jsonl");
    const std::string original = "id,name\n1,alpha\n2,\"be,ta\"\n";
    write_file(temp_path("test_export_src3.csv"), original);
    OpenOptions options;
    options.use_sidecar = false;
    auto dataset = Dataset::open(temp_path("test_export_src3.csv"), options);
    const auto json_stats = io::export_rows(*dataset, nullptr, {}, path, io::OutputFormat::JsonLines, {});
    CHECK_EQ(json_stats.rows, std::uint64_t{2});
    const std::string jsonl = read_back(path);
    CHECK_EQ(jsonl, std::string("{\"id\":1,\"name\":\"alpha\"}\n{\"id\":2,\"name\":\"be,ta\"}\n"));
    std::remove(temp_path("test_export.jsonl").c_str());
    std::remove(temp_path("test_export_src3.csv").c_str());
}

TEST(export_respects_row_selection_and_columns) {
    const std::string path = temp_path("test_export_subset.csv");
    const std::string original = "id,name,units\n1,alpha,3\n2,beta,4\n3,gamma,5\n";
    write_file(temp_path("test_export_src4.csv"), original);
    OpenOptions options;
    options.use_sidecar = false;
    auto dataset = Dataset::open(temp_path("test_export_src4.csv"), options);
    std::vector<std::uint64_t> rows{0, 2};
    std::vector<std::uint32_t> columns{0, 2};
    const auto subset_stats = io::export_rows(*dataset, &rows, columns, path, io::OutputFormat::Csv, {});
    CHECK_EQ(subset_stats.rows, std::uint64_t{2});
    CHECK_EQ(read_back(path), std::string("id,units\n1,3\n3,5\n"));
    std::remove(temp_path("test_export_subset.csv").c_str());
    std::remove(temp_path("test_export_src4.csv").c_str());
}

TEST(cancellation_stops_an_export) {
    const std::string path = temp_path("test_export_cancel.csv");
    write_file(temp_path("test_export_src5.csv"), "id\n1\n2\n3\n");
    OpenOptions options;
    options.use_sidecar = false;
    auto dataset = Dataset::open(temp_path("test_export_src5.csv"), options);
    CancelToken token;
    token.cancel();
    query::RunOptions run;
    run.cancel = &token;
    CHECK_THROWS(io::export_rows(*dataset, nullptr, {}, path, io::OutputFormat::Csv, run));
    std::remove(temp_path("test_export_cancel.csv").c_str());
    std::remove(temp_path("test_export_src5.csv").c_str());
}

TEST(write_result_emits_csv_and_table) {
    query::ResultSet result;
    result.headers = {"region", "n"};
    result.types = {ColumnType::Text, ColumnType::Integer};
    result.rows = {{"EU", "2"}, {"US", "1"}};

    std::ostringstream csv;
    io::write_result(csv, result, io::OutputFormat::Csv);
    CHECK_EQ(csv.str(), std::string("region,n\nEU,2\nUS,1\n"));

    std::ostringstream table;
    io::write_result(table, result, io::OutputFormat::Csv);
    CHECK(table.str().find("EU") != std::string::npos);
}

TEST(output_format_parser_accepts_aliases) {
    CHECK_EQ(std::string(io::to_string(io::parse_output_format("csv"))), std::string("csv"));
    CHECK_EQ(std::string(io::to_string(io::parse_output_format("TSV"))), std::string("tsv"));
    CHECK_EQ(std::string(io::to_string(io::parse_output_format("json"))), std::string("jsonl"));
    CHECK_EQ(std::string(io::to_string(io::parse_output_format("jsonl"))), std::string("jsonl"));
    CHECK_THROWS(io::parse_output_format("parquet"));
}
