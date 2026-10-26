// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_query.cpp
//  Parser and executor coverage for the documented SQL subset. The corpus is
//  intentionally small and hand-checkable: if one of these numbers changes,
//  either the executor regressed or the SQL semantics changed on purpose.
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "core/dataset.hpp"
#include "query/exec.hpp"
#include "query/parser.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace tess;
using namespace tess::query;

namespace {

std::shared_ptr<Dataset> corpus() {
    static const std::shared_ptr<Dataset> dataset = Dataset::parse(
        "id,region,units,revenue,active,created,note\n"
        "1,EU,3,1200.50,true,2024-01-31,alpha\n"
        "2,US,7,980.00,false,2024-02-01,beta\n"
        "3,EU,11,2500.25,true,2024-03-15,\n"
        "4,APAC,2,99.99,true,2023-12-01,gamma\n"
        "5,US,9,1750.00,false,2024-04-20,delta\n",
        "corpus.csv");
    return dataset;
}

ResultSet run(const std::string& sql) {
    std::string error;
    auto query = parse_query(sql, *corpus(), error);
    if (!query.has_value()) throw testing::Failure{"parse error: " + error};
    return execute(*corpus(), *query, {});
}

std::vector<std::uint64_t> matching(const std::string& expression) {
    std::string error;
    auto predicate = parse_expression(expression, *corpus(), error);
    if (!predicate) throw testing::Failure{"parse error: " + error};
    return filter_rows(*corpus(), *predicate, {});
}

} // namespace

TEST(select_star_preserves_shape) {
    const ResultSet result = run("select * from t");
    CHECK_EQ(result.rows.size(), std::size_t{5});
    CHECK_EQ(result.headers.size(), std::size_t{7});
    CHECK_EQ(result.headers[0], std::string("id"));
    CHECK_EQ(result.scanned, std::uint64_t{5});
}

TEST(select_projection_and_alias) {
    const ResultSet result = run("select region as area, units from t limit 2");
    CHECK_EQ(result.headers.size(), std::size_t{2});
    CHECK_EQ(result.headers[0], std::string("area"));
    CHECK_EQ(result.rows.size(), std::size_t{2});
    CHECK_EQ(result.rows[0][0], std::string("EU"));
    CHECK_EQ(result.rows[0][1], std::string("3"));
    CHECK(result.truncated);
}

TEST(where_comparisons_and_boolean_operators) {
    CHECK_EQ(matching("units > 4").size(), std::size_t{3});
    CHECK_EQ(matching("units >= 4 and revenue < 1800").size(), std::size_t{2});
    CHECK_EQ(matching("region = 'EU' or region = 'US'").size(), std::size_t{4});
    CHECK_EQ(matching("active = true").size(), std::size_t{3});
    CHECK_EQ(matching("note = ''").size(), std::size_t{1});
    CHECK_EQ(matching("not (region = 'EU')").size(), std::size_t{3});
}

TEST(where_like_and_in_and_between) {
    CHECK_EQ(matching("note like 'a%'").size(), std::size_t{1});
    CHECK_EQ(matching("note like '%a'").size(), std::size_t{4});
    CHECK_EQ(matching("region in ('EU','APAC')").size(), std::size_t{3});
    CHECK_EQ(matching("units between 3 and 9").size(), std::size_t{3});
    CHECK_EQ(matching("region is not null").size(), std::size_t{5});
}

TEST(where_timestamps_and_arithmetic) {
    CHECK_EQ(matching("created >= '2024-02-01'").size(), std::size_t{3});
    CHECK_EQ(matching("units * 2 > 15").size(), std::size_t{2});
    CHECK_EQ(matching("revenue / units > 300").size(), std::size_t{1});
    CHECK_EQ(matching("units % 2 = 1").size(), std::size_t{4});
}

TEST(aggregates_over_the_whole_table) {
    const ResultSet result = run("select count(*) n, sum(units) units, avg(revenue) avg from t");
    CHECK_EQ(result.rows.size(), std::size_t{1});
    CHECK_EQ(result.rows[0][0], std::string("5"));
    CHECK_EQ(result.rows[0][1], std::string("32"));
    CHECK_NEAR(std::stod(result.rows[0][2]), 1306.148, 0.01);
}

TEST(aggregates_ignore_nulls) {
    const ResultSet result = run("select count(note) n, count(*) total from t");
    CHECK_EQ(result.rows[0][0], std::string("4"));
    CHECK_EQ(result.rows[0][1], std::string("5"));
}

TEST(group_by_with_ordering) {
    const ResultSet result = run(
        "select region, count(*) n, sum(revenue) rev from t group by region order by rev desc");
    CHECK_EQ(result.rows.size(), std::size_t{3});
    CHECK_EQ(result.rows[0][0], std::string("EU"));
    CHECK_EQ(result.rows[0][1], std::string("2"));
    CHECK_EQ(result.rows[1][0], std::string("US"));
    CHECK_EQ(result.groups, std::uint64_t{3});
}

TEST(having_is_rejected_with_guidance) {
    std::string error;
    auto query = parse_query("select region, count(*) from t group by region having count(*) > 1",
                             *corpus(), error);
    CHECK(!query.has_value());
    CHECK(error.find("HAVING") != std::string::npos);
}

TEST(order_by_multiple_keys_and_direction) {
    const ResultSet result = run("select id from t order by region asc, units desc");
    std::vector<std::string> ids;
    for (const auto& row : result.rows) ids.push_back(row[0]);
    CHECK_EQ(ids.front(), std::string("4")); // APAC
    CHECK_EQ(ids.back(), std::string("2"));  // US, units 7
    const auto eu = std::find(ids.begin(), ids.end(), "3");
    CHECK(eu != ids.end());
}

TEST(limit_and_offset) {
    const ResultSet result = run("select id from t order by id limit 2 offset 2");
    CHECK_EQ(result.rows.size(), std::size_t{2});
    CHECK_EQ(result.rows[0][0], std::string("3"));
    CHECK_EQ(result.rows[1][0], std::string("4"));
}

TEST(distinct_removes_duplicate_rows) {
    const ResultSet result = run("select distinct region from t order by region");
    CHECK_EQ(result.rows.size(), std::size_t{3});
    CHECK_EQ(result.rows[0][0], std::string("APAC"));
}

TEST(expressions_in_select) {
    const ResultSet result = run("select units * 2 as doubled, upper(region) as shout from t limit 1");
    CHECK_EQ(result.headers[0], std::string("doubled"));
    CHECK_EQ(result.rows[0][0], std::string("6"));
    CHECK_EQ(result.rows[0][1], std::string("EU"));
}

TEST(parse_errors_name_the_column) {
    std::string error;
    auto query = parse_query("select revenuee from t", *corpus(), error);
    CHECK(!query.has_value());
    CHECK(error.find("revenuee") != std::string::npos);
    CHECK(error.find("revenue") != std::string::npos);
}

TEST(parse_errors_are_specific) {
    std::string error;
    CHECK(!parse_query("select * from t where", *corpus(), error).has_value());
    CHECK(!error.empty());
    error.clear();
    CHECK(!parse_query("select from t", *corpus(), error).has_value());
    error.clear();
    CHECK(!parse_query("nonsense", *corpus(), error).has_value());
}

TEST(bare_column_with_aggregate_is_rejected) {
    // `select region, count(*) from t` collapses the rows that gave `region`
    // its value. Guessing ("the first row's region") would return a number
    // that looks authoritative and is meaningless, so it must be an error.
    std::string error;
    CHECK(!parse_query("select region, count(*) from t", *corpus(), error).has_value());
    CHECK(error.find("GROUP BY") != std::string::npos);

    error.clear();
    CHECK(!parse_query("select *, count(*) from t", *corpus(), error).has_value());
    CHECK(error.find("GROUP BY") != std::string::npos);

    // An expression containing a bare column is refused too, not just a plain one.
    error.clear();
    CHECK(!parse_query("select upper(region), sum(revenue) from t", *corpus(), error).has_value());

    // Constants and pure aggregate expressions are well defined without GROUP BY.
    error.clear();
    CHECK(parse_query("select count(*), sum(revenue) from t", *corpus(), error).has_value());
    CHECK(parse_query("select sum(revenue) / count(*) from t", *corpus(), error).has_value());
    CHECK(parse_query("select count(*), 'total' as label from t", *corpus(), error).has_value());

    // Grouping the bare column makes it legal again.
    CHECK(parse_query("select region, count(*) from t group by region", *corpus(), error).has_value());
}

TEST(first_row_of_each_group_supplies_ungrouped_columns) {
    // Documented consequence of grouping: a column that is neither grouped nor
    // aggregated reports the first row in its group, deterministically.
    const ResultSet result = run("select region, units from t group by region order by region");
    CHECK_EQ(result.rows.size(), std::size_t{3});
    CHECK_EQ(result.rows[0][0], std::string("APAC"));
    CHECK_EQ(result.rows[0][1], std::string("2")); // APAC's first row is id 4, units 2
    CHECK_EQ(result.rows[1][0], std::string("EU"));
    CHECK_EQ(result.rows[1][1], std::string("3")); // EU's first row is id 1, units 3
}

TEST(operators_documented_in_the_reference_behave) {
    // `||` concatenates, `%` is the remainder, and division by zero is NULL
    // rather than an error.
    const ResultSet concatenated = run("select region || '-' || note as tag from t limit 1");
    CHECK_EQ(concatenated.rows[0][0], std::string("EU-alpha"));

    const ResultSet remainder = run("select units % 4 from t limit 1");
    CHECK_EQ(remainder.rows[0][0], std::string("3"));

    const ResultSet divided = run("select units / 0 as bad from t limit 1");
    CHECK(divided.rows[0][0].empty());
}

TEST(order_by_position_refers_to_select_item) {
    const ResultSet by_position = run("select region, count(*) from t group by region order by 2 desc");
    CHECK_EQ(by_position.rows.size(), std::size_t{3});
    CHECK_EQ(by_position.rows[0][0], std::string("EU"));
    CHECK_EQ(by_position.rows[0][1], std::string("2"));
}

TEST(functions_on_columns) {
    CHECK_EQ(matching("lower(region) = 'eu'").size(), std::size_t{2});
    CHECK_EQ(matching("length(note) > 4").size(), std::size_t{3});
    CHECK_EQ(matching("round(revenue, 0) >= 1750").size(), std::size_t{2});
    CHECK_EQ(matching("coalesce(note, 'none') = 'none'").size(), std::size_t{1});
}

TEST(like_is_case_insensitive_and_escaped) {
    CHECK(like_match("hello", "HEL%"));
    CHECK(like_match("a_b", "a\\_b"));
    CHECK(!like_match("axb", "a\\_b"));
    CHECK(like_match("abc", "a_c"));
    CHECK(!like_match("abc", "a_"));
}

TEST(filter_rows_reports_scanned_count) {
    const auto rows = matching("region = 'EU'");
    CHECK_EQ(rows.size(), std::size_t{2});
    CHECK_EQ(rows[0], std::uint64_t{0});
    CHECK_EQ(rows[1], std::uint64_t{2});
}

TEST(search_rows_is_case_insensitive) {
    const auto rows = search_rows(*corpus(), "GAMMA", {}, {});
    CHECK_EQ(rows.size(), std::size_t{1});
    CHECK_EQ(rows[0], std::uint64_t{3});
}

TEST(sort_rows_is_stable_and_null_last) {
    auto rows = all_rows(*corpus());
    std::string error;
    auto expression = parse_expression("note", *corpus(), error);
    CHECK(expression != nullptr);
    std::vector<OrderKey> keys;
    keys.push_back(OrderKey{std::move(expression), false});
    sort_rows(*corpus(), rows, keys, {});
    CHECK_EQ(rows.size(), std::size_t{5});
    // The empty note (row 3) sorts last in ascending order.
    CHECK_EQ(rows.back(), std::uint64_t{2});
}
