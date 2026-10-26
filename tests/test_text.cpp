// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_text.cpp
//  Numeric parsing/formatting and terminal-text handling. These functions are
//  called for every rendered cell and every typed filter, so a locale-dependent
//  bug here would be invisible until someone's laptop was set to de_DE.
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "common/util.hpp"

using namespace tess;

TEST(parse_int_accepts_signs_and_digits) {
    std::int64_t value = 0;
    CHECK(parse_int("42", value));
    CHECK_EQ(value, 42);
    CHECK(parse_int("-17", value));
    CHECK_EQ(value, -17);
    CHECK(parse_int("+9", value));
    CHECK_EQ(value, 9);
    CHECK(!parse_int("4.2", value));
    CHECK(!parse_int("", value));
    CHECK(parse_int(" 42", value));
    CHECK_EQ(value, 42);
    CHECK(parse_int("7 ", value));
    CHECK_EQ(value, 7);
}

TEST(parse_double_is_locale_independent) {
    double value = 0.0;
    CHECK(parse_double("2400.50", value));
    CHECK_NEAR(value, 2400.5, 1e-9);
    CHECK(parse_double("-1.5e3", value));
    CHECK_NEAR(value, -1500.0, 1e-9);
    CHECK(parse_double(".5", value));
    CHECK_NEAR(value, 0.5, 1e-12);
    CHECK(parse_double("2.", value));
    CHECK_NEAR(value, 2.0, 1e-12);
    CHECK(!parse_double("1,000", value));
    CHECK(!parse_double("", value));
}

TEST(parse_bool_covers_common_spellings) {
    bool value = false;
    CHECK(parse_bool("true", value));
    CHECK(value);
    CHECK(parse_bool("TRUE", value));
    CHECK(value);
    CHECK(parse_bool("yes", value));
    CHECK(value);
    CHECK(parse_bool("0", value));
    CHECK(!value);
    CHECK(parse_bool("no", value));
    CHECK(!value);
    CHECK(!parse_bool("maybe", value));
}

TEST(format_count_uses_thousands_separators) {
    CHECK_EQ(format_count(0), std::string("0"));
    CHECK_EQ(format_count(999), std::string("999"));
    CHECK_EQ(format_count(1000), std::string("1,000"));
    CHECK_EQ(format_count(1234567), std::string("1,234,567"));
    CHECK_EQ(format_count(1000000000ULL), std::string("1,000,000,000"));
}

TEST(format_bytes_picks_readable_units) {
    CHECK_EQ(format_bytes(0), std::string("0 B"));
    CHECK_EQ(format_bytes(999), std::string("999 B"));
    CHECK_EQ(format_bytes(1024), std::string("1.0 KB"));
    CHECK_EQ(format_bytes(1536), std::string("1.5 KB"));
    CHECK_EQ(format_bytes(5ULL * 1024 * 1024 * 1024), std::string("5.0 GB"));
}

TEST(format_double_keeps_significant_digits) {
    CHECK_EQ(format_double(2400.0), std::string("2400"));
    CHECK_EQ(format_double(0.000123456), std::string("0.000123456"));
    CHECK_EQ(format_double(10600.16876, 8), std::string("10600.169"));
}

TEST(parse_timestamp_accepts_iso_variants) {
    const auto date = parse_timestamp("2024-01-31");
    CHECK(date.has_value());
    CHECK_EQ(format_timestamp(*date, true), std::string("2024-01-31"));

    const auto stamp = parse_timestamp("2024-01-31 10:22:03");
    CHECK(stamp.has_value());
    CHECK_EQ(format_timestamp(*stamp), std::string("2024-01-31 10:22:03"));

    const auto zulu = parse_timestamp("2024-01-31T10:22:03Z");
    CHECK(zulu.has_value());
    CHECK_EQ(*zulu, *stamp);

    const auto offset = parse_timestamp("2024-01-31T12:22:03+02:00");
    CHECK(offset.has_value());
    CHECK_EQ(*offset, *stamp);

    CHECK(!parse_timestamp("31/01/2024").has_value());
    CHECK(!parse_timestamp("2024-13-01").has_value());
}

TEST(utf8_measurement_handles_wide_and_combining) {
    CHECK_EQ(text_width("hello"), 5);
    CHECK_EQ(text_width("héllo"), 5);
    CHECK_EQ(rune_count("héllo"), std::size_t{5});
    CHECK_EQ(text_width("日本"), 4);      // two wide runes
    CHECK_EQ(rune_count("日本"), std::size_t{2});
    CHECK_EQ(text_width("e\xCC\x81"), 1); // e + combining acute
    CHECK_EQ(text_width("a\x1b[31mb"), 7); // ESC renders as one middle dot
}

TEST(truncate_width_respects_display_columns) {
    CHECK_EQ(truncate_width("abcdef", 4), std::string("abc…"));
    CHECK_EQ(truncate_width("abcd", 4), std::string("abcd"));
    CHECK_EQ(truncate_width("日本語", 4), std::string("日…"));
    CHECK_EQ(truncate_width("anything", 0), std::string());
}

TEST(sanitize_neutralises_control_bytes) {
    const std::string hostile = "abc\x1b[2J" "def\nghi";
    const std::string safe = sanitize(hostile);
    CHECK(safe.find('\x1b') == std::string::npos);
    CHECK(safe.find('\n') == std::string::npos);
    CHECK_EQ(safe, std::string("abc·[2Jdef·ghi"));
}

TEST(ellipsize_path_keeps_the_file_name) {
    const std::string short_path = "data/sales.csv";
    CHECK_EQ(ellipsize_path(short_path, 40), short_path);
    const std::string long_path = ellipsize_path("/home/someone/very/deep/corpus/sales_2024.csv", 24);
    CHECK(text_width(long_path) <= 24);
    CHECK(long_path.find("sales_2024.csv") != std::string::npos);
}

TEST(case_insensitive_helpers) {
    CHECK(iequals("Region", "region"));
    CHECK(!iequals("Region", "region "));
    CHECK(starts_with_icase("Total Revenue", "total"));
    CHECK(contains_icase("North America", "AMER"));
    CHECK_EQ(to_lower("MiXeD"), std::string("mixed"));
    CHECK_EQ(to_upper("MiXeD"), std::string("MIXED"));
}

TEST(split_and_join_round_trip) {
    const auto parts = split("a,b,,c", ',');
    CHECK_EQ(parts.size(), std::size_t{4});
    CHECK_EQ(std::string(parts[2]), std::string());
    CHECK_EQ(join({"a", "b", "c"}, "/"), std::string("a/b/c"));
}

TEST(trim_only_strips_ascii_whitespace) {
    CHECK_EQ(std::string(trim("  x\t\n")), std::string("x"));
    CHECK_EQ(std::string(trim("   ")), std::string());
    CHECK_EQ(std::string(lstrip("  x  ")), std::string("x  "));
    CHECK_EQ(std::string(rstrip("  x  ")), std::string("  x"));
}

TEST(number_appenders_match_formatters) {
    std::string out;
    append_count(out, 1234567);
    CHECK_EQ(out, std::string("1,234,567"));
    out.clear();
    append_bytes(out, 1536);
    CHECK_EQ(out, std::string("1.5 KB"));
}
