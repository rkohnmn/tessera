// ─────────────────────────────────────────────────────────────────────────────
//  Tessera · tests/test_sidecar.cpp
//  The `.tessera.idx` sidecar is the one file on disk that gets *read back* and
//  believed. These tests treat it as untrusted input: truncated, byte-flipped,
//  stale, and deliberately forged by someone who knows the format and wants a
//  crash or a 4 GiB allocation. Every one of them must end in "reject and
//  rescan", never in a crash, a hang, or a wrong answer.
//
//  The field offsets below are duplicated from sidecar.cpp on purpose. If the
//  layout changes, these tests stop compiling meaningfully (the magic version is
//  asserted), which is exactly the alarm wanted: a silent format change is how a
//  stale cache becomes a wrong row count in someone's spreadsheet.
// ─────────────────────────────────────────────────────────────────────────────
#include "harness.hpp"

#include "core/dataset.hpp"
#include "core/sidecar.hpp"
#include "core/source.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace tess;
using testing::temp_path;

namespace {

// ── Layout, mirrored from src/core/sidecar.cpp ──────────────────────────────
constexpr std::size_t kMagicSize = 8;
constexpr std::size_t kDataSizeAt = 12;
constexpr std::size_t kChecksumFrom = 36;
constexpr std::size_t kRowCountAt = 44;
constexpr std::size_t kHeaderSize = 52; // magic..row_count; the payload follows
constexpr std::size_t kFooterSize = 8;  // trailing checksum
constexpr std::uint32_t kVersion = 4;

void poke_u64(std::string& bytes, std::size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xFFu);
}

/// Recomputes the trailing checksum, so a test can mutate a field and still
/// present a file that a format-aware attacker could have produced. Without this
/// the checksum would do the rejecting and the structural checks would never be
/// exercised. The checksum field itself must already be reserved at the end.
void repair_checksum(std::string& bytes) {
    const std::size_t signed_bytes = bytes.size() - kFooterSize;
    poke_u64(bytes, signed_bytes, fnv1a(std::string_view(bytes.data() + kChecksumFrom, signed_bytes - kChecksumFrom)));
}

void put_varint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

/// Builds a sidecar exactly the way sidecar.cpp does, but from arbitrary header
/// fields and an arbitrary payload, so a forged-but-checksum-consistent file can
/// be tested. The checksum is computed with the same public FNV-1a the reader
/// uses, which is the strongest thing an attacker could manage.
std::string forge(std::uint64_t data_size, std::int64_t mtime, std::uint64_t signature, std::uint64_t row_count,
                  std::string_view payload) {
    std::string out;
    out.append("TESSIDX4", kMagicSize);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((kVersion >> (8 * i)) & 0xFFu));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((data_size >> (8 * i)) & 0xFFu));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((static_cast<std::uint64_t>(mtime) >> (8 * i)) & 0xFFu));
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((signature >> (8 * i)) & 0xFFu));
    out.push_back(',');
    out.push_back('"');
    out.push_back(1);
    out.append(5, '\0');
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((row_count >> (8 * i)) & 0xFFu));
    out.append(payload);
    out.append(kFooterSize, '\0');
    repair_checksum(out);
    return out;
}

/// True when a sidecar with this payload is rejected. The cache goes next to a
/// real data file, because the reader looks for `<data>.tessera.idx` — writing
/// the forgery to the data path instead would make every rejection pass for the
/// wrong reason (no cache at all).
bool rejects_payload(std::uint64_t data_size, std::uint64_t row_count, std::string_view payload) {
    const std::string path = temp_path("sidecar_forged.csv");
    write_file(path, "id,name\n1,alpha\n2,beta\n");
    write_file(index_cache_path(path), forge(data_size, 111, 222, row_count, payload));
    return !load_index_cache(path, data_size, 111, 222).has_value();
}

} // namespace

TEST(sidecar_layout_constants_still_match_the_reader) {
    // A save/load pair is only meaningful if both agree on the byte layout the
    // tests above hand-craft. This pins the two together.
    const std::string path = temp_path("sidecar_layout.csv");
    const std::vector<std::uint64_t> offsets{0, 10, 25, 100};
    CHECK(save_index_cache(path, 500, 42, 0xDEADBEEF, offsets, ';', '"', true));
    const std::string bytes = read_file(index_cache_path(path));
    CHECK_EQ(bytes.size() >= kHeaderSize + kFooterSize, true);
    CHECK_EQ(bytes.compare(0, kMagicSize, std::string("TESSIDX4")), 0);
    CHECK_EQ(static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[kDataSizeAt])), std::uint64_t{500} & 0xFFu);
    CHECK_EQ(static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[kRowCountAt])), std::uint64_t{4});
}

TEST(sidecar_round_trips_offsets) {
    const std::string path = temp_path("sidecar_round.csv");
    const std::vector<std::uint64_t> offsets{0, 9, 20, 33, 1000};
    CHECK(save_index_cache(path, 4096, 7, 0x1234, offsets, ',', '"', false));

    IndexCacheInfo info;
    const auto loaded = load_index_cache(path, 4096, 7, 0x1234, &info);
    CHECK(loaded.has_value());
    CHECK(*loaded == offsets);
    CHECK_EQ(info.row_count, std::uint64_t{5});
    CHECK_EQ(info.bytes, file_size(index_cache_path(path)).value_or(0));
}

TEST(sidecar_rejects_stale_size_mtime_or_signature) {
    const std::string path = temp_path("sidecar_stale.csv");
    const std::vector<std::uint64_t> offsets{0, 5, 12};
    CHECK(save_index_cache(path, 100, 555, 0xABC, offsets, ',', '"', true));
    // The file grew: the cached offsets are no longer valid.
    CHECK(!load_index_cache(path, 101, 555, 0xABC).has_value());
    // The file was rewritten in place at the same size: mtime catches it.
    CHECK(!load_index_cache(path, 100, 556, 0xABC).has_value());
    // Same size and mtime but a different header: a different table entirely.
    CHECK(!load_index_cache(path, 100, 555, 0xABD).has_value());
}

TEST(sidecar_rejects_truncation_and_flipped_bytes) {
    const std::string path = temp_path("sidecar_corrupt.csv");
    const std::vector<std::uint64_t> offsets{0, 5, 11, 40};
    CHECK(save_index_cache(path, 100, 3, 9, offsets, ',', '"', true));
    const std::string good = read_file(index_cache_path(path));

    for (const std::size_t cut : {std::size_t{0}, kHeaderSize - 1, kHeaderSize, good.size() - 1}) {
        write_file(index_cache_path(path), good.substr(0, cut));
        CHECK(!load_index_cache(path, 100, 3, 9).has_value());
    }

    // One changed byte anywhere in the header or payload must be caught, either
    // by the checksum or by one of the structural checks.
    for (std::size_t at = 0; at < good.size(); ++at) {
        std::string damaged = good;
        damaged[at] = static_cast<char>(static_cast<unsigned char>(damaged[at]) ^ 0x40u);
        write_file(index_cache_path(path), damaged);
        CHECK(!load_index_cache(path, 100, 3, 9).has_value());
    }
}

TEST(sidecar_rejects_an_impossible_row_count_without_allocating) {
    const std::string path = temp_path("sidecar_liar.csv");
    const std::vector<std::uint64_t> offsets{0, 4, 9};
    CHECK(save_index_cache(path, 64, 1, 1, offsets, ',', '"', true));

    // A count far larger than the payload could possibly encode. The reader must
    // reject it on arithmetic: reserving for it would be a multi-gigabyte
    // allocation driven by a five-byte file.
    for (const std::uint64_t lie : {1ull << 20, 1ull << 40, (1ull << 63), ~0ull}) {
        std::string bytes = read_file(index_cache_path(path));
        poke_u64(bytes, kRowCountAt, lie);
        repair_checksum(bytes); // a forger who knows the format would do this
        write_file(index_cache_path(path), bytes);
        CHECK(!load_index_cache(path, 64, 1, 1).has_value());
    }

    // A count larger than the file itself can never be honest: every row is at
    // least one byte on disk.
    std::string bytes = read_file(index_cache_path(path));
    poke_u64(bytes, kRowCountAt, 65);
    repair_checksum(bytes);
    write_file(index_cache_path(path), bytes);
    CHECK(!load_index_cache(path, 64, 1, 1).has_value());
}

TEST(sidecar_rejects_forged_offsets_that_overflow_or_regress) {
    // delta near UINT64_MAX: naively accumulated it wraps to a small, plausible
    // offset. Rejected because the reader subtracts before adding.
    std::string overflow_payload;
    put_varint(overflow_payload, 0);
    put_varint(overflow_payload, ~0ull);
    CHECK(rejects_payload(1000, 2, overflow_payload));

    // Two rows at the same offset: a record can never start twice.
    std::string flat_payload;
    put_varint(flat_payload, 7);
    put_varint(flat_payload, 0);
    CHECK(rejects_payload(1000, 2, flat_payload));

    // Offsets that walk off the end of the file.
    std::string past_end;
    put_varint(past_end, 0);
    put_varint(past_end, 1000);
    CHECK(rejects_payload(1000, 2, past_end));

    // More offsets than payload bytes: each varint costs at least one byte.
    CHECK(rejects_payload(1000, 5, std::string("\x00\x01", 2)));

    // Payload exhausted mid-record.
    CHECK(rejects_payload(1000, 2, std::string("\x00", 1)));

    // Trailing bytes the header does not account for.
    std::string trailing;
    put_varint(trailing, 0);
    put_varint(trailing, 9);
    trailing.push_back('\x00');
    CHECK(rejects_payload(1000, 2, trailing));

    // An offset beyond 63 bits is a malformed varint, not a large row number.
    std::string wild;
    put_varint(wild, 0);
    for (int i = 0; i < 9; ++i) wild.push_back(static_cast<char>(0xFF));
    wild.push_back(static_cast<char>(0x7F));
    CHECK(rejects_payload(1000, 2, wild));

    // The same payloads with an honest count and a sane file must still load:
    // the rejections above have to come from the pattern, not from the harness.
    std::string honest;
    put_varint(honest, 0);
    put_varint(honest, 10);
    put_varint(honest, 10);
    CHECK(!rejects_payload(1000, 3, honest));
}

TEST(sidecar_write_leaves_no_temporary_files_behind) {
    const std::string path = temp_path("sidecar_atomic.csv");
    const std::string cache = index_cache_path(path);
    const std::vector<std::uint64_t> offsets{0, 3, 8, 12};
    std::remove(cache.c_str());
    CHECK(save_index_cache(path, 50, 1, 1, offsets, ',', '"', true));

    // Overwriting repeatedly must not accumulate scratch files next to the data,
    // and the cache itself must always load after a write.
    for (int i = 0; i < 5; ++i) {
        CHECK(save_index_cache(path, 50, 1, 1, offsets, ',', '"', true));
        CHECK(load_index_cache(path, 50, 1, 1).has_value());
    }
    std::size_t siblings = 0;
    const std::filesystem::path directory = std::filesystem::path(cache).parent_path();
    const std::string stem = std::filesystem::path(cache).filename().string();
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(stem, 0) == 0) ++siblings;
    }
    CHECK_EQ(siblings, std::size_t{1});
}

TEST(sidecar_refuses_empty_or_garbage_caches) {
    const std::string path = temp_path("sidecar_limits.csv");
    write_file(index_cache_path(path), "");
    CHECK(!load_index_cache(path, 10, 1, 1).has_value());

    // A cache file far larger than any honest index of this input is refused
    // before it is read, so corruption cannot become an out-of-memory kill.
    std::string huge(4096, '\0');
    write_file(index_cache_path(path), huge);
    CHECK(!load_index_cache(path, 10, 1, 1).has_value());

    std::remove(index_cache_path(path).c_str());
    CHECK(!load_index_cache(path, 10, 1, 1).has_value()); // missing file
}

TEST(sidecar_read_helpers_bound_and_report_sizes) {
    const std::string path = temp_path("helpers_size.bin");
    const std::string payload(5000, 'x');
    write_file(path, payload);
    CHECK_EQ(file_size(path).value_or(0), std::uint64_t{5000});
    CHECK_EQ(read_file_limited(path, 5000).value_or("").size(), std::size_t{5000});
    CHECK(!read_file_limited(path, 4999).has_value());        // one byte over the cap
    CHECK(!file_size(temp_path("helpers_missing.bin")).has_value());
    CHECK(!file_size(temp_path(".")).has_value()); // a directory is not a cache file

    // read_file must report an exact size through the 64-bit seek path.
    CHECK_EQ(read_file(path).size(), payload.size());
}

TEST(sidecar_corruption_is_rebuilt_rather_than_fatal) {
    // The end-to-end promise: a damaged cache costs a rescan, nothing more. The
    // CLI never tells the user about the cache unless it worked.
    const std::string path = temp_path("sidecar_rebuild.csv");
    write_file(path, "id,name,amount\n1,alpha,10\n2,beta,20\n3,gamma,30\n");
    std::remove(index_cache_path(path).c_str());

    OpenOptions write_once;
    write_once.use_sidecar = true;
    write_once.write_sidecar = true;
    const auto first = Dataset::open(path, write_once);
    CHECK_EQ(first->row_count(), std::uint64_t{3});
    CHECK(first->info().index_from_sidecar);

    // Truncate the cache to a third of its length: the header survives, the
    // payload does not, which is the most dangerous shape of corruption.
    const std::string good = read_file(index_cache_path(path));
    write_file(index_cache_path(path), good.substr(0, good.size() / 3));

    OpenOptions read_cache;
    read_cache.use_sidecar = true;
    read_cache.write_sidecar = false;
    const auto rebuilt = Dataset::open(path, read_cache);
    CHECK_EQ(rebuilt->row_count(), std::uint64_t{3});
    CHECK(!rebuilt->info().index_from_sidecar);
    CHECK_EQ(rebuilt->decode_cell(2, 1), std::string("gamma"));

    // Forged row count, honest checksum: still rescanned, still correct.
    std::string lied = good;
    poke_u64(lied, kRowCountAt, 1ull << 45);
    repair_checksum(lied);
    write_file(index_cache_path(path), lied);
    const auto again = Dataset::open(path, read_cache);
    CHECK_EQ(again->row_count(), std::uint64_t{3});
    CHECK(!again->info().index_from_sidecar);
    CHECK_EQ(again->decode_cell(1, 2), std::string("20"));

    // And a restored cache is used again, so the recovery is not one-way.
    CHECK(save_index_cache(path, file_size(path).value_or(0), 0, 0, {0, 20, 40, 60}, ',', '"', true));
    std::remove(index_cache_path(path).c_str());
    (void)Dataset::open(path, write_once);
    const auto cached = Dataset::open(path, read_cache);
    CHECK(cached->info().index_from_sidecar);
    CHECK_EQ(cached->row_count(), std::uint64_t{3});
}
