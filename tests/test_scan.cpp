// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_scan.cpp
//  Format detection and the CSV scanner. Every heuristic in the product is
//  pinned here because detection mistakes are the kind of bug users blame on
//  "the tool can't read my file".
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "core/dataset.hpp"
#include "core/dialect.hpp"

#include <string>
#include <vector>

using namespace tess;
using testing::temp_path;

namespace {

template <typename Enum>
std::string name(Enum value) {
    return std::string(to_string(value));
}


SniffResult sniff_text(const std::string& text, std::string extension = ".csv") {
    SniffOptions options;
    options.extension = std::move(extension);
    return sniff(text.data(), text.size(), options);
}

} // namespace

TEST(sniff_comma_delimited) {
    const auto result = sniff_text("region,units,revenue\nEU,3,1200.50\nUS,7,980.00\n");
    CHECK_EQ(result.dialect.delimiter, ',');
    CHECK(result.dialect.has_header);
    CHECK_EQ(result.first_record_fields, std::uint32_t{3});
}

TEST(sniff_semicolon_delimited_european_export) {
    const auto result = sniff_text("region;units;revenue\nEU;3;1200,50\nUS;7;980,00\n");
    CHECK_EQ(result.dialect.delimiter, ';');
    CHECK(result.dialect.has_header);
}

TEST(sniff_tab_separated_by_extension) {
    const auto result = sniff_text("region\tunits\trevenue\nEU\t3\t1200\n", ".tsv");
    CHECK_EQ(result.dialect.delimiter, '\t');
}

TEST(sniff_pipe_delimited) {
    const auto result = sniff_text("a|b|c\n1|2|3\n4|5|6\n");
    CHECK_EQ(result.dialect.delimiter, '|');
}

TEST(sniff_ignores_delimiters_inside_quotes) {
    const auto result = sniff_text("id,note\n1,\"a,b,c,d\"\n2,\"e,f,g,h\"\n");
    CHECK_EQ(result.dialect.delimiter, ',');
    CHECK_EQ(result.first_record_fields, std::uint32_t{2});
}

TEST(sniff_headerless_numeric_file) {
    const auto result = sniff_text("1,2,3\n4,5,6\n7,8,9\n");
    CHECK_EQ(result.dialect.delimiter, ',');
    CHECK(!result.dialect.has_header);
}

TEST(sniff_detects_crlf) {
    const auto result = sniff_text("a,b\r\n1,2\r\n3,4\r\n");
    CHECK(result.dialect.crlf);
    CHECK_EQ(result.dialect.delimiter, ',');
}

TEST(sniff_bom_is_recorded_and_not_part_of_first_name) {
    std::string text = "\xEF\xBB\xBFregion,units\nEU,3\n";
    const auto result = sniff_text(text, ".csv");
    CHECK_EQ(name(result.dialect.encoding), name(Encoding::Utf8Bom));
    CHECK_EQ(result.dialect.bom_bytes, std::uint64_t{3});
}

TEST(utf16_transcoding_produces_utf8) {
    // "a,b\n1,2\n" in UTF-16LE, with a BOM.
    std::string le = "\xFF\xFE";
    for (char c : std::string("a,b\n1,2\n")) {
        le.push_back(c);
        le.push_back('\0');
    }
    CHECK_EQ(name(detect_encoding(le.data(), le.size())), name(Encoding::Utf16LE));
    std::uint64_t bom = 0;
    const std::string utf8 = utf16_to_utf8(le, true, bom);
    CHECK_EQ(utf8, std::string("a,b\n1,2\n"));
    CHECK_EQ(bom, std::uint64_t{2});
}

TEST(split_fields_handles_quotes_and_doubled_quotes) {
    std::vector<FieldSpan> spans;
    split_fields("1,\"a,b\",\"he said \"\"hi\"\"\"", ',', '"', spans);
    CHECK_EQ(spans.size(), std::size_t{3});
    CHECK_EQ(std::string(decode_field("a,b", true, '"')), std::string("a,b"));
    CHECK_EQ(std::string(decode_field("he said \"\"hi\"\"", true, '"')), std::string("he said \"hi\""));
}

TEST(count_fields_stops_at_line_end) {
    CHECK_EQ(count_fields("a,b,c", ',', '"'), std::uint32_t{3});
    CHECK_EQ(count_fields("a,\"b,c\",d", ',', '"'), std::uint32_t{3});
    CHECK_EQ(count_fields("", ',', '"'), std::uint32_t{1});
}

TEST(csv_escaping_round_trips) {
    std::string out;
    append_csv_field(out, "plain", ',', '"');
    CHECK_EQ(out, std::string("plain"));
    out.clear();
    append_csv_field(out, "has,comma", ',', '"');
    CHECK_EQ(out, std::string("\"has,comma\""));
    out.clear();
    append_csv_field(out, "line\nbreak", ',', '"');
    CHECK_EQ(out, std::string("\"line\nbreak\""));
    out.clear();
    append_csv_field(out, "say \"hi\"", ',', '"');
    CHECK_EQ(out, std::string("\"say \"\"hi\"\"\""));
}

TEST(json_string_escaping) {
    std::string out;
    append_json_string(out, "a\"b\\c\n\t");
    CHECK_EQ(out, std::string("\"a\\\"b\\\\c\\n\\t\""));
}

TEST(dataset_parses_types_and_nulls) {
    const auto dataset = Dataset::parse(
        "id,region,units,price,active,created,note\n"
        "1,EU,3,12.50,true,2024-01-31,\n"
        "2,US,4,9.99,false,2024-02-01,\"a, b\"\n",
        "<test>");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(dataset->column_count(), std::uint32_t{7});
    CHECK_EQ(std::string(dataset->column_name(0)), std::string("id"));
    CHECK_EQ(name(dataset->column_type(2)), name(ColumnType::Integer));
    CHECK_EQ(name(dataset->column_type(3)), name(ColumnType::Float));
    CHECK_EQ(name(dataset->column_type(4)), name(ColumnType::Boolean));
    CHECK_EQ(name(dataset->column_type(5)), name(ColumnType::Timestamp));

    CHECK_EQ(dataset->cell_int(0, 2).value_or(-1), 3);
    CHECK_NEAR(dataset->cell_double(1, 3).value_or(0.0), 9.99, 1e-9);
    CHECK(!dataset->cell(0, 6).size());
    CHECK_EQ(dataset->decode_cell(1, 6), std::string("a, b"));

    const auto index = dataset->column_index("price");
    CHECK(index.has_value());
    CHECK_EQ(*index, std::uint32_t{3});
    CHECK(!dataset->column_index("nope").has_value());
}

TEST(dataset_row_text_is_byte_exact) {
    const std::string text = "a,b\n\"x, y\",2\n";
    const auto dataset = Dataset::parse(text, "<test>");
    CHECK_EQ(std::string(dataset->row_text(0)), std::string("\"x, y\",2"));
    CHECK_EQ(dataset->row_offset(0), std::uint64_t{4});
    CHECK_EQ(dataset->decode_cell(0, 0), std::string("x, y"));
}

TEST(dataset_counts_ragged_rows_without_failing) {
    const auto dataset = Dataset::parse("a,b,c\n1,2,3\n4,5\n6,7,8,9\n", "<test>");
    CHECK_EQ(dataset->row_count(), std::uint64_t{3});
    // The widest record wins (4 fields), so both shorter rows are ragged and
    // their missing cells read as empty rather than shifting the schema.
    CHECK_EQ(dataset->column_count(), std::uint32_t{4});
    CHECK_EQ(dataset->info().ragged_rows, std::uint64_t{2});
    CHECK_EQ(std::string(dataset->cell(1, 2)), std::string());
    CHECK_EQ(std::string(dataset->cell(2, 3)), std::string("9"));
}

TEST(dataset_parses_jsonl_with_union_of_keys) {
    const auto dataset = Dataset::parse(
        "{\"id\":1,\"region\":\"EU\"}\n"
        "{\"id\":2,\"units\":3}\n"
        "{\"id\":3,\"region\":\"US\",\"units\":5}\n",
        "events.jsonl");
    CHECK_EQ(name(dataset->info().format), name(Format::JsonLines));
    CHECK_EQ(dataset->row_count(), std::uint64_t{3});
    CHECK(dataset->column_count() >= 3);
    const auto region = dataset->column_index("region");
    const auto units = dataset->column_index("units");
    CHECK(region.has_value());
    CHECK(units.has_value());
    CHECK_EQ(std::string(dataset->cell(0, *region)), std::string("EU"));
    CHECK_EQ(std::string(dataset->cell(1, *units)), std::string("3"));
    CHECK(!dataset->cell(1, *region).size());
}

TEST(dataset_handles_multiline_quoted_values) {
    const auto dataset = Dataset::parse("id,note\n1,\"line one\nline two\"\n2,plain\n", "<test>");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(dataset->decode_cell(0, 1), std::string("line one\nline two"));
    CHECK_EQ(dataset->decode_cell(1, 1), std::string("plain"));
}

TEST(dataset_header_override) {
    OpenOptions options;
    options.header = false;
    const auto dataset = Dataset::parse("1,2\n3,4\n", "<test>", options);
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(std::string(dataset->column_name(0)), std::string("column_1"));
}

TEST(sidecar_index_round_trip) {
    const std::string path = temp_path("test_sidecar.csv");
    write_file(path, "id,name\n1,alpha\n2,beta\n3,gamma\n");
    OpenOptions first;
    first.use_sidecar = false;
    auto original = Dataset::open(path, first);
    const std::uint64_t written = original->save_index_cache();
    CHECK(written > 0);

    OpenOptions second;
    second.use_sidecar = true;
    auto cached = Dataset::open(path, second);
    CHECK(cached->info().index_from_sidecar);
    CHECK_EQ(cached->row_count(), original->row_count());
    CHECK_EQ(cached->decode_cell(2, 1), std::string("gamma"));
    std::remove(path.c_str());
    std::remove((path + ".tessera.idx").c_str());
}
