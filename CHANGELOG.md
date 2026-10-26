# Changelog

All notable changes to Tessera are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] — 2026-10-26

First release. Everything below is new, so this entry is a description of the shape of the program
rather than a diff.

### Core engine

* Memory-mapped, read-only file access (`mmap` on POSIX, `CreateFileMapping`/`MapViewOfFile` on
  Windows) with an owned-buffer mode for transcoded UTF-16 and materialised JSON Lines.
* Parallel row index — one 8-byte offset per record — built with a quote-parity scheme that is
  correct in the presence of embedded newlines and doubled quotes, with a repairing sequential
  fallback for malformed files.
* Format and encoding detection for delimited files and JSON Lines: UTF-8 (with or without BOM),
  UTF-16LE/BE, Latin-1 fallback, delimiter agreement scoring, quote detection, CRLF/LF, and header
  detection — each decision recorded as a human-readable note that `info` prints.
* Lazy per-column type inference (text, integer, float, boolean, timestamp) with null and ragged-row
  tracking, and byte-exact raw row text.
* Optional `.tessera.idx` sidecar cache: delta + LEB128 offsets, validated against file size, mtime
  and a header signature; a stale cache is rejected rather than trusted. Written best-effort so a
  read-only directory degrades to "slower", never "broken".
* Streaming column profiling with bounded memory: fill rate, nulls, min/max/mean/stddev,
  p25/median/p75/p99, a 72-bucket histogram, text-length statistics, and top-N values, with distinct
  counts from a 2¹⁴-register HyperLogLog.

### Query engine

* Hand-written lexer and recursive-descent parser for a documented, frozen SQL subset over a single
  table `t`: `SELECT` (`*`, expressions, aliases, `DISTINCT`), `WHERE`, `GROUP BY`, `ORDER BY`
  (multi-key, `ASC`/`DESC`, positional), `LIMIT`/`OFFSET`, and the bare-expression form used by the
  TUI filter bar.
* Operators: `= <> != < <= > >=`, `AND`/`OR`/`NOT`, `LIKE`/`NOT LIKE` (case-insensitive, `%`/`_`,
  backslash escapes), `IN`, `BETWEEN`, `IS NULL`/`IS NOT NULL`, arithmetic with `||` concatenation
  and `%` remainder.
* Aggregates: `count`, `count(*)`, `count_distinct`, `sum`, `avg`/`mean`, `min`, `max`; scalars for
  text, number and date manipulation; `coalesce`/`ifnull`; conversions.
* Semantics chosen for usefulness and documented: aliases resolve in `WHERE` as well as `GROUP BY`
  and `ORDER BY`; a bare column beside an aggregate without `GROUP BY` is an error rather than a
  silently meaningless value; division by zero yields NULL; NULL comparisons are false.
* Errors that name the problem and suggest the fix, with the statement echoed and the position marked.

### Interface

* Interactive explorer (TUI) with a diffing cell renderer: column sizing to content, type row, cursor
  and scroll state, `j k h l`, arrows, paging, `g`/`G`, live search (`/`), SQL-subset filter (`f`),
  sort menu (`s`), per-column profiles (`p`, `P`), inline command line (`:`), export of the current
  view (`e`), clipboard copy (`y`), reload (`r`), type-row toggle (`t`), help (`?`).
* Every long operation runs on a worker thread with progress and `Esc` cancellation; the UI never
  blocks.
* Unicode-correct layout: display-width measurement for CJK and combining marks, control-byte
  sanitisation, UTF-16 Windows console input.
* POSIX `termios` and Win32 console implementations behind one interface; alternate screen buffer,
  24-bit colour, 256/16-colour and no-colour fallbacks.
* One-shot CLI verbs: `info`, `head`, `tail`, `stats`, `query`, `export`, `index`, `bench`, `help`,
  `version`, with `-n -c -q -o -f -d --header --no-header --threads --cache-index --no-index
  --sample --no-color`.
* Output adapts to its destination: a table on a terminal, clean CSV in a pipe; progress to stderr
  only when attached to a terminal.

### I/O

* Streaming CSV, TSV and JSONL writers with correct escaping; export honours the current filter and
  sort so what you see is what you get.
* Table renderer with display-width alignment, and profile cards with histogram bars.

### Quality

* 113 unit tests over the parts where bugs are silent: quoting and escaping, BOM and UTF-16
  transcoding, delimiter detection, type inference, byte-exact row round-tripping, the index and its
  sidecar, parser and executor behaviour, HyperLogLog estimation, timestamp parsing, UTF-8 widths,
  sanitisation and format writers. Thirty-three of them are files people actually receive: bank
  exports in UTF-16, European semicolon CSVs, pipe dumps, blank lines from a broken writer, duplicate
  column names from a join, an unterminated quote, a corrupt JSON line. Thirteen more cover the
  argument layer — the exit code and the exact sentence printed for an unknown flag, a non-numeric
  row count, a flag with no value, a multi-character delimiter and an unreadable file — because a
  command line is the first thing a new user types and the last thing a test suite tends to reach.
* Warning-clean under GCC's, Clang's and Zig-clang's full warning sets with `-Werror`.
* CI builds with GCC and Clang, runs the suite under AddressSanitizer and UndefinedBehaviorSanitizer
  on both, fuzzes the parser, and cross-compiles the Windows executable.
* `scripts/build_windows.sh` produces a statically linked `PE32+` binary importing only Windows
  system libraries, from Linux or macOS, via Zig.
* `scripts/gen_data.py` generates byte-identical benchmark corpora for reproducible numbers.
* Measured, reproducible benchmarks and baselines in `bench/`, reported in
  [docs/BENCHMARKS.md](docs/BENCHMARKS.md).

### Verification of the shipped Windows executable

* `dist/tessera.exe` is not merely compiled and inspected — it is executed. Under Wine 10.0,
  `scripts/verify_windows_runtime.sh` runs 87 checks comparing it against the Linux binary built from
  the same sources: every CLI verb, six runtime failure paths and seven argument-parsing paths with
  their exit codes and messages, a semicolon CSV with accented content, an emoji filename, sidecar
  write/reuse/recovery on a non-ASCII path, a read-only directory, UTF-8 output bytes, and a 113 MB
  filtered export plus a full group-by over the 1.1 GB 10-million-row corpus, all byte-identical
  across the two builds. Timings are the only values permitted to differ; the large-file section
  reports itself as skipped, loudly, when the corpus has not been generated.
* Three real defects were found by running it and fixed here: narrow `fopen`/`ifstream` on a UTF-8
  Windows path (which wrote a cache under a mangled name while reporting success); link-time
  optimisation on `x86_64-windows-gnu` producing incomplete exception tables, so some errors printed
  `libc++abi: terminating due to uncaught exception` and exited 3 instead of exiting 1 with a
  message (`-flto` is no longer used for that target); and — on every platform, not just Windows —
  `parse_options` being called outside the `try` in `cli::run`, so any rejected argument reached
  `std::terminate` and aborted (exit 134 on Linux, 3 on Windows) instead of printing one line and
  exiting 1. The first two were invisible to reading; the third was hidden by a check list that only
  diffed errors raised from inside the engine, which is now covered by seven tests and seven runtime
  comparisons.
* Native MSVC CI runs the same binary on a real Windows runner and asserts the error exit codes, the
  non-ASCII path round trip, cache recovery from a corrupted sidecar, and plain UTF-8 in a pipe.
* `scripts/gen_package_manifests.py` generates winget, Scoop and Homebrew manifests with real release
  hashes; `docs/INSTALL.md` records the state of each submission honestly.

### Hardening

* The `.tessera.idx` sidecar is now treated as untrusted input: bounded read, checksum over the
  descriptor fields and the payload, format version in the magic, declared row count validated
  against both the payload and the data file before any allocation, deltas compared against the
  remaining file size before being added (no accumulator wrap), strict monotonicity and range checks,
  no trailing bytes, and an atomic write. `tests/test_sidecar.cpp` forges checksum-valid but
  internally inconsistent files and requires each to be rejected.
* Adversarial corpus and fuzzing harnesses: `scripts/gen_edge_cases.py`, `tests/test_edge_cases.cpp`,
  `tests/fuzz_tessera.py` (differential against Python's `csv`, plus mutation), and
  `scripts/verify_sanitizers.sh`. Failures save their input under `artifacts/fuzz-crashes/`.
* Windows console setup (UTF-8 code page, virtual-terminal processing) now applies to the CLI verbs as
  well as the explorer, so one-shot output matches Linux byte for byte.
* Single-column files are recognised as a first-class case rather than depending on a delimiter being
  present, a leading blank line no longer confuses header detection, and 64-bit file sizes are used
  throughout (`ftell` saturates at 2 GiB on Windows, which would have made large caches unreadable
  there).
