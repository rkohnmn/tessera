// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_edge_cases.cpp
//  The files people actually receive: bank exports in UTF-16, European
//  semicolon CSVs, pipe dumps, blank lines from a broken writer, duplicate
//  column names from a join, an unterminated quote, a corrupt JSON line.
//
//  Each case here was a real failure at some point, or is one the scanner is
//  specifically designed to survive. They are pinned so a later "cleanup" of
//  the detector cannot quietly reintroduce them.
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "core/dataset.hpp"
#include "core/dialect.hpp"
#include "core/source.hpp"

#include <string>
#include <vector>

using namespace tess;
using testing::temp_path;

namespace {

/// Writes `bytes` to a scratch file and opens it the way the CLI does, so the
/// encoding path (which needs a real file to sniff and transcode) is exercised.
std::shared_ptr<Dataset> open_bytes(const std::string& bytes, const std::string& name) {
    const std::string path = temp_path(name);
    write_file(path, bytes);
    return Dataset::open(path, OpenOptions{});
}

std::string cell(const Dataset& dataset, std::uint64_t row, std::uint32_t column) {
    return dataset.decode_cell(row, column);
}

} // namespace

TEST(edge_utf8_bom_is_not_part_of_the_first_column) {
    const auto dataset = open_bytes("\xef\xbb\xbfid,name\n1,alpha\n2,beta\n", "edge_bom.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(std::string(dataset->column_name(0)), std::string("id"));
    CHECK_EQ(cell(*dataset, 0, 1), std::string("alpha"));
}

TEST(edge_utf16_little_endian_with_bom) {
    // "id,name\n1,alpha\n2,beta\n" in UTF-16LE, as some bank exports arrive.
    const std::u16string text = u"id,name\n1,alpha\n2,beta\n";
    std::string bytes = "\xff\xfe";
    for (const char16_t ch : text) {
        bytes.push_back(static_cast<char>(ch & 0xFF));
        bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
    }
    const auto dataset = open_bytes(bytes, "edge_utf16le.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(std::string(dataset->column_name(1)), std::string("name"));
    CHECK_EQ(cell(*dataset, 1, 1), std::string("beta"));
}

TEST(edge_utf16_big_endian_with_bom) {
    const std::u16string text = u"id,name\n1,alpha\n";
    std::string bytes = "\xfe\xff";
    for (const char16_t ch : text) {
        bytes.push_back(static_cast<char>((ch >> 8) & 0xFF));
        bytes.push_back(static_cast<char>(ch & 0xFF));
    }
    const auto dataset = open_bytes(bytes, "edge_utf16be.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{1});
    CHECK_EQ(cell(*dataset, 0, 1), std::string("alpha"));
}

TEST(edge_latin1_bytes_survive) {
    // "Zürich", "São Paulo", "Malmö" in Latin-1: not valid UTF-8, and the file
    // must still open with its text visible rather than aborting.
    std::string bytes = "id,city\n1,Z\xfcrich\n2,S\xe3o Paulo\n3,Malm\xf6\n";
    const auto dataset = open_bytes(bytes, "edge_latin1.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{3});
    CHECK(cell(*dataset, 0, 1).find("rich") != std::string::npos);
}

TEST(edge_cjk_and_emoji_are_preserved) {
    const auto cjk = open_bytes("id,city\n1,\xe6\x9d\xb1\xe4\xba\xac\n", "edge_cjk.csv");
    CHECK_EQ(cjk->row_count(), std::uint64_t{1});
    // UTF-8 for 東京, unchanged.
    CHECK_EQ(cell(*cjk, 0, 1), std::string("\xe6\x9d\xb1\xe4\xba\xac"));

    const auto emoji = open_bytes("id,note\n1,ship it \xf0\x9f\x9a\x80\n", "edge_emoji.csv");
    CHECK_EQ(cell(*emoji, 0, 1), std::string("ship it \xf0\x9f\x9a\x80"));
}

TEST(edge_delimiter_variants_are_detected) {
    const auto semi = open_bytes("id;name;amount\n1;alpha;10,5\n2;beta;20,0\n", "edge_semi.csv");
    CHECK_EQ(semi->column_count(), std::uint32_t{3});
    CHECK_EQ(semi->info().delimiter, ';');
    CHECK_EQ(semi->row_count(), std::uint64_t{2});

    const auto pipe = open_bytes("id|name|amount\n1|alpha|10\n2|beta|20\n", "edge_pipe.csv");
    CHECK_EQ(pipe->info().delimiter, '|');
    CHECK_EQ(pipe->row_count(), std::uint64_t{2});

    const auto tab = open_bytes("id\tname\n1\talpha\n", "edge_tab.tsv");
    CHECK_EQ(tab->info().delimiter, '\t');
    CHECK_EQ(tab->row_count(), std::uint64_t{1});
}

TEST(edge_quoted_fields_with_commas_and_doubled_quotes) {
    const auto dataset =
        open_bytes("\"id\",\"name\"\n\"1\",\"alpha, inc.\"\n\"2\",\"say \"\"hi\"\"\"\n", "edge_quoted.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(cell(*dataset, 0, 1), std::string("alpha, inc."));
    CHECK_EQ(cell(*dataset, 1, 1), std::string("say \"hi\""));
}

TEST(edge_embedded_newlines_inside_quotes) {
    const auto dataset =
        open_bytes("id,note\n1,\"line one\nline two\"\n2,\"crlf\r\ninside\"\n3,plain\n", "edge_multiline.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{3});
    CHECK_EQ(cell(*dataset, 0, 1), std::string("line one\nline two"));
    CHECK_EQ(cell(*dataset, 2, 1), std::string("plain"));
}

TEST(edge_crlf_and_missing_final_newline) {
    const auto crlf = open_bytes("id,name\r\n1,alpha\r\n2,beta\r\n", "edge_crlf.csv");
    CHECK_EQ(crlf->row_count(), std::uint64_t{2});
    CHECK_EQ(cell(*crlf, 1, 1), std::string("beta"));

    const auto bare = open_bytes("id,name\n1,alpha\n2,beta", "edge_notrailing.csv");
    CHECK_EQ(bare->row_count(), std::uint64_t{2});
    CHECK_EQ(cell(*bare, 1, 1), std::string("beta"));
}

TEST(edge_degenerate_files_do_not_crash) {
    const auto empty = open_bytes("", "edge_empty.csv");
    CHECK_EQ(empty->row_count(), std::uint64_t{0});

    const auto bom_only = open_bytes("\xef\xbb\xbf", "edge_bomonly.csv");
    CHECK_EQ(bom_only->row_count(), std::uint64_t{0});

    const auto header_only = open_bytes("id,name,amount\n", "edge_headeronly.csv");
    CHECK_EQ(header_only->row_count(), std::uint64_t{0});
    CHECK_EQ(header_only->column_count(), std::uint32_t{3});

    const auto single_column = open_bytes("value\n1\n2\n3\n4\n", "edge_single.csv");
    CHECK_EQ(single_column->row_count(), std::uint64_t{4});
    CHECK_EQ(single_column->column_count(), std::uint32_t{1});
}

TEST(edge_blank_records_are_skipped) {
    // A trailing double newline or a spacer row must not become a data row.
    const auto dataset = open_bytes("id,name\n1,alpha\n\n2,beta\n\n\n", "edge_blank.csv");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(cell(*dataset, 0, 0), std::string("1"));
    CHECK_EQ(cell(*dataset, 1, 0), std::string("2"));

    const auto leading = open_bytes("\n\nid,name\n1,alpha\n", "edge_blank_lead.csv");
    CHECK_EQ(leading->row_count(), std::uint64_t{1});
    CHECK_EQ(std::string(leading->column_name(0)), std::string("id"));
}

TEST(edge_duplicate_header_names_are_made_unique) {
    // Exports of joined tables routinely repeat a column name. The file still
    // has a header, and both columns stay addressable.
    const auto dataset = open_bytes("id,id,amount\n1,2,10\n3,4,20\n", "edge_dupheader.csv");
    CHECK(dataset->info().has_header);
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    CHECK_EQ(std::string(dataset->column_name(0)), std::string("id"));
    CHECK_EQ(std::string(dataset->column_name(1)), std::string("id_2"));
    CHECK_EQ(cell(*dataset, 1, 1), std::string("4"));
}

TEST(edge_ragged_rows_and_padded_cells) {
    const auto ragged = open_bytes("id,name,amount\n1,alpha,10\n2,beta\n3,gamma,30,extra\n", "edge_ragged.csv");
    CHECK_EQ(ragged->row_count(), std::uint64_t{3});
    CHECK_EQ(cell(*ragged, 1, 2), std::string("")); // missing trailing field reads as null

    const auto padded = open_bytes("id, name , amount\n1, alpha , 10\n", "edge_padded.csv");
    CHECK_EQ(std::string(padded->column_name(1)), std::string("name"));
    CHECK_EQ(cell(*padded, 0, 1), std::string(" alpha ")); // value padding is preserved verbatim
}

TEST(edge_leading_zeros_are_text_not_numbers) {
    const auto dataset = open_bytes("zip,code\n01234,0001\n99999,0002\n", "edge_zeros.csv");
    CHECK_EQ(cell(*dataset, 0, 0), std::string("01234"));
    CHECK_EQ(cell(*dataset, 1, 1), std::string("0002"));
}

TEST(edge_nul_byte_and_large_field) {
    std::string with_nul = "id,name\n1,a";
    with_nul.push_back('\0');
    with_nul += "b\n2,beta\n";
    const auto nul = open_bytes(with_nul, "edge_nul.csv");
    CHECK_EQ(nul->row_count(), std::uint64_t{2});

    std::string big = "id,blob\n1,";
    big.append(200000, 'x');
    big += "\n2,small\n";
    const auto large = open_bytes(big, "edge_largefield.csv");
    CHECK_EQ(large->row_count(), std::uint64_t{2});
    CHECK_EQ(cell(*large, 0, 1).size(), std::size_t{200000});
}

TEST(edge_unbalanced_quotes_are_repaired_with_a_warning) {
    // A stray quote must not swallow the rest of the file, and the repair has to
    // be reported: silently pretending the file is fine is worse than saying so.
    const auto dataset = open_bytes("id,note\n1,\"unbalanced\n2,beta\n", "edge_badquote.csv");
    CHECK(dataset->row_count() >= 1);
    bool warned = false;
    for (const auto& note : dataset->info().notes) {
        if (note.find("unbalanced quotes") != std::string::npos) warned = true;
    }
    CHECK(warned);
}

TEST(edge_jsonl_union_of_keys_and_nesting) {
    const auto dataset =
        open_bytes("{\"id\":1,\"name\":\"alpha\"}\n{\"id\":2,\"extra\":true}\n{\"id\":3,\"n\":3.5}\n",
                   "edge_union.jsonl");
    CHECK_EQ(dataset->row_count(), std::uint64_t{3});
    // Every key seen anywhere becomes a column.
    bool has_name = false;
    bool has_extra = false;
    for (std::uint32_t c = 0; c < dataset->column_count(); ++c) {
        const std::string name(dataset->column_name(c));
        if (name == "name") has_name = true;
        if (name == "extra") has_extra = true;
    }
    CHECK(has_name);
    CHECK(has_extra);
}

TEST(edge_jsonl_malformed_record_is_reported_not_hidden) {
    const auto dataset = open_bytes("{\"a\":1}\nnot json at all\n{\"a\":3}\n", "edge_bad.jsonl");
    CHECK_EQ(dataset->row_count(), std::uint64_t{2});
    bool reported = false;
    for (const auto& note : dataset->info().notes) {
        if (note.find("malformed JSON") != std::string::npos) reported = true;
    }
    CHECK(reported);
}
