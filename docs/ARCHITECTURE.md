# Architecture

Tessera is a single process with one shared engine and two front ends. Everything important happens
in `tess::Dataset`, and both the CLI verbs and the TUI are thin shells over it. This document
describes the modules, the data flow, the threading model and the on-disk index format.

```
                       ┌──────────────────────────────────────────┐
   file on disk ──────▶│ Source   (mmap / owned buffer)           │
                       └───────────────┬──────────────────────────┘
                                       │  const char* data, size
                       ┌───────────────▼──────────────────────────┐
                       │ Dialect  encoding · delimiter · header   │  detection, with notes
                       └───────────────┬──────────────────────────┘
                                       │
                       ┌───────────────▼──────────────────────────┐
                       │ RowIndex   8 bytes per record            │  parallel, quote-parity
                       └───────────────┬──────────────────────────┘
                                       │  std::vector<uint64_t> offsets
              ┌────────────────────────┼────────────────────────┐
              │                        │                        │
     ┌────────▼────────┐     ┌─────────▼─────────┐    ┌─────────▼─────────┐
     │ Decode / types  │     │ query::execute    │    │ profile_column    │
     │ Dataset         │     │ filter·group·sort │    │ HLL·quantiles·hist│
     └────────┬────────┘     └─────────┬─────────┘    └─────────┬─────────┘
              │                        │                        │
     ┌────────▼────────────────────────▼────────────────────────▼─────────┐
     │ io::  CSV / TSV / JSONL / table rendering, profile cards           │
     └────────┬───────────────────────────────────────────┬───────────────┘
              │                                           │
     ┌────────▼─────────┐                     ┌───────────▼──────────────┐
     │ cli::commands    │                     │ tui::App  (worker jobs)  │
     │ one-shot verbs   │                     │ grid · prompts · views   │
     └──────────────────┘                     └──────────────────────────┘
```

## Modules

### `src/common/`

* **`error.hpp`** — one exception type, `tess::Error`, carrying a human-readable message. Every
  failure path in the program throws it and the outermost frame turns it into an exit code, so
  user-visible errors are always a single clear sentence rather than a stack trace. `cli::run` is the
  only frame that catches, so everything that can throw — argument parsing included — has to be
  called from inside its `try`; a throw outside it reaches `std::terminate`. That requirement is
  spelled out at the call site and enforced by `tests/test_cli.cpp`. Also defines `CancelToken`,
  `ProgressFn` and `ThreadPool`-free job plumbing shared by every long operation.
* **`util.hpp` / `util.cpp`** — `now_seconds()`, locale-independent `parse_int` / `parse_double` /
  `parse_bool` / `parse_timestamp`, `format_count` / `format_bytes` / `format_double`, UTF-8
  display-width measurement, `truncate_width`, `sanitize` (control-byte neutralisation), case
  folding, splitting and joining. Locale independence matters: a machine with a comma decimal
  separator must not change how numbers parse.

### `src/core/`

* **`source.hpp` / `source.cpp`** — the only place that talks to the operating system about files.
  `Source::open_file` memory-maps read-only (`mmap` on POSIX, `CreateFileMapping` + `MapViewOfFile` on
  Windows); `Source::from_buffer` owns a heap buffer, used for UTF-16 files transcoded to UTF-8 and
  for rows materialised out of JSONL. Open is O(1) in file size — the header, not the data, is all
  that is touched. Exposes bytes, size, name, path, `mtime` and a `view(offset, length)` accessor.
* **`simd.hpp`** — header-only SWAR (SIMD Within A Register) primitives: `find_byte`,
  `find_either`, `count_byte`, `any_byte_lt`, all over 64-bit words, portable and intrinsic-free.
  These are the innermost loops of indexing, scanning and line counting.
* **`dialect.hpp` / `dialect.cpp`** — format detection. Decides encoding (UTF-8 with or without BOM,
  UTF-16LE/BE, Latin-1 fallback), then format (delimited vs JSON Lines), then for delimited files
  the delimiter, quote character, CRLF-vs-LF and whether the first record is a header. Each decision
  appends a human-readable line to `Dialect::notes`, which is what `tessera info` prints — the
  question "why did it pick `;`?" is answered by the program itself instead of a support thread.
* **`dataset.hpp` / `dataset.cpp`** — the heart of the engine. `Dataset::open` runs detection, builds
  or loads the row index, samples records for type inference, resolves the header, and exposes:
  `row_count`, `column_count`, `column_name`, `column_type`, `row_view(row)`, `decode_cell(row, col)`,
  and byte-exact raw row text. It also tracks ragged rows, null counts and per-column type confidence.
  Cell decoding is lazy: nothing is converted to `int64`/`double` until a query or view asks.
* **`indexer.hpp` / `indexer.cpp`** — the parallel row index (see [below](#the-row-index)).
* **`sidecar.hpp` / `sidecar.cpp`** — the optional `.tessera.idx` cache: delta + LEB128 encoded
  offsets, plus magic and format version, file size, mtime, an FNV-1a header signature, and an FNV-1a
  checksum over the descriptor fields and the payload. A cache is used only when every one of those
  matches, so a stale index is never trusted; the read is size-bounded, the declared row count is
  checked against both the payload and the data file before any allocation, deltas are compared
  against the remaining file size *before* being added so they cannot wrap, and the write is atomic
  (temporary file plus rename). Writes are best-effort and never throw — a read-only directory just
  means the cache is skipped, and a rejected cache means a rescan rather than a wrong row count.
* **`profile.hpp` / `profile.cpp`** — streaming column profiling in one pass with bounded memory:
  fill rate, nulls, min/max/mean/stddev, p25/median/p75/p99 (reservoir sampling), a 72-bucket
  histogram, min/mean/max text length, and top-N values. Distinct counts come from a 2¹⁴-register
  **HyperLogLog** (~0.8% standard error) with merge support, so grouped and sampled profiling cost
  the same fixed memory regardless of file size.

### `src/query/`

* **`parser.hpp` / `parser.cpp`** — a hand-written recursive-descent lexer and parser for the frozen
  SQL subset. The parser resolves column names against the open `Dataset` as it parses, which is why
  a typo can report *"no column named `regoin`; did you mean `region`?"* at parse time rather than
  failing later. `parse_expression` reuses the same path for the TUI's filter bar, so filters and
  queries are the same language by construction.
* **`exec.hpp` / `exec.cpp`** — execution over the index. Because the index is a sorted array of
  record offsets, filtering is a parallel scan producing a `std::vector<uint64_t>` of row numbers,
  and every later stage operates on row numbers rather than data. `execute` handles projection,
  `WHERE`, aggregation with `GROUP BY`, `ORDER BY` (stable, multi-key, with a fast path for
  already-ordered single keys) and `LIMIT`/`OFFSET`. Sorting is `std::stable_sort` over row numbers
  with a typed comparator, so a sorted result never copies row bytes.

### `src/io/`

* **`format.hpp` / `format.cpp`** — result rendering. `emit_result` prints an aligned, coloured table
  on a terminal and clean CSV when piped; `render_profiles` draws the profile cards including the
  histogram bars. Alignment uses display width, not byte length, so CJK content still lines up.
  Also holds the progress-bar renderer.
* **`writer.hpp` / `writer.cpp`** — streaming writers for CSV, TSV and JSONL, with correct escaping
  (doubled quotes, embedded newlines and control-character escaping per format) and byte accounting
  for the export summary.

### `src/cli/`

Argument intake (`main.cpp`), option parsing and the one-shot verbs (`commands.cpp`). Every verb
opens a `Dataset`, does one thing, prints, and exits — no shared mutable state, no server, no
long-running process. `--version` and `help` share a single version banner with the TUI.

### `src/tui/`

* **`terminal.hpp` / `terminal.cpp`** — raw-mode setup, key decoding (including arrow and function
  keys, which arrive as escape sequences), resize events, clipboard copy, and alternate-screen
  handling, implemented twice: POSIX `termios`/`ioctl` and the Win32 console API. Windows input is
  read as UTF-16 and converted, so non-ASCII keys and pasted text work.
* **`screen.hpp` / `screen.cpp`** — a cell grid with per-cell attributes and a **diffing** flusher
  that emits escape codes only for cells that changed since the previous frame, batching runs that
  share attributes. This is what keeps a 200-column grid smooth over a slow SSH connection.
* **`theme.hpp`** — colour palette, glyph sets and the box/bar characters, with a no-colour fallback.
* **`app.hpp` / `app.cpp`** — application state and the event loop: cursor and scroll position, view
  transform (filter expression, sort key, search text), prompt editing with Unicode-aware cursor
  movement, background jobs, toasts, and the keymap. All heavy work runs in `BackgroundJob` on a
  `std::thread` with a `CancelToken` and an atomic progress value, so the UI thread never blocks and
  `Esc` cancels.
* **`views.cpp`** — pure rendering: the grid with its header, type row and column sizing; the record
  detail pane; the profile overlay; the sort menu; the help screen; the status and hint bars.

## The row index

The index is the only per-file structure in memory: one `std::uint64_t` per physical record, sorted,
starting at the first record after any BOM.

Building it in parallel is the interesting part. CSV quoting is a state machine — a scan cannot know
whether a byte offset begins inside a quoted field without knowing how many quote characters
preceded it. The naive fixes are to scan sequentially (slow) or to guess and then repair chunk
boundaries (fragile, and wrong on files with embedded newlines).

Tessera uses a property of well-formed CSV: **every record contains an even number of quote
characters.** Unquoted fields contribute none, and a quoted field contributes exactly two plus a pair
for each doubled quote. Therefore the parity of the total quote count in the prefix
`[begin, offset)` tells us the quoting state at `offset` exactly.

1. **Pass 1** splits the file into chunks aligned to arbitrary byte boundaries (not records) and
   counts quote characters in each using `swar::count_byte`, in parallel. Chunk `i` starts inside a
   quoted field iff the sum of quote counts of chunks `0…i-1` is odd.
2. **Pass 2** scans each chunk again, in parallel, from its known starting state, emitting the
   offsets of record starts into a per-chunk vector.
3. The per-chunk vectors are concatenated in order into the final offsets array.

No locks, no coordination, no boundary repair, and correct in the presence of embedded newlines and
doubled quotes. Two cheap passes over memory bandwidth cost far less than a serialised one, which is
where the 1.29 GB/s (11 M rows/s) figure comes from.

Two safety valves:

* If the file's total quote count is **odd**, the file is malformed. Rather than producing a
  nonsense index, the engine falls back to `build_row_index_sequential` with repair enabled, which
  abandons a runaway quote after 1 MiB without a closing quote instead of letting it swallow the
  rest of the file, and reports that it did so.
* If the concatenated offsets are somehow not sorted, they are sorted and de-duplicated before use.
  A corrupted parity calculation must never produce an unusable index.

When a sidecar cache is present and valid it replaces both passes entirely, which is why a 10-million
row file re-opens in 0.17 s instead of 1.08 s.

## Concurrency and cancellation

There is no thread pool and no global state. Each job owns its threads, and the pattern is uniform:

* Work is split by *data range* or by *row range*, and a shared `std::atomic<std::size_t>` hands out
  the next unit — work-stealing without a queue, which keeps chunks of uneven difficulty balanced.
* A `CancelToken` is checked at chunk boundaries and periodically inside long inner loops; `Esc` in
  the TUI sets it and the worker unwinds through the standard exception path.
* Progress is an `std::atomic<double>` plus a phase string, read by the UI thread when it draws. No
  mutex is held while drawing.
* Results are published to the UI only once, on the UI thread, via an `apply` callback that runs when
  the job completes.

The CLI has exactly the same machinery, with a progress callback that writes to stderr only when it
is attached to a terminal — which is why piping `tessera query` gives clean CSV with no progress
noise.

## Data flow for a query

```
parse_query(sql, dataset)          resolve columns, reject typos with suggestions
        │
        ├─ no WHERE, no GROUP BY ─────────▶ stream rows in index order
        │
        └─ WHERE ──▶ parallel scan of index ranges ──▶ vector<row>
                                                       │
                       GROUP BY ──▶ hash aggregation ──┤
                                                       │
                       ORDER BY  ──▶ stable sort ──────┤
                                                       │
                       LIMIT/OFFSET ──▶ slice ─────────┘
                                                       │
                                    emit_result ──▶ table or CSV
```

Only `LIMIT`-bounded, group-aligned or fully materialised results ever hold decoded values; the rest
of the pipeline moves 8-byte row numbers.

## Portability

Two things differ between platforms and both are isolated behind one header each: file mapping
(`core/source.cpp`) and terminal I/O (`tui/terminal.cpp`). Everywhere else the code is standard C++20
plus POSIX threads, which MSVC satisfies through `Threads::Threads` in CMake. `main.cpp` has a
`wmain` on Windows so paths and arguments arrive as UTF-16 and are converted once, at the boundary.

The result of that isolation is that the Windows executable is produced by the same translation
units that Linux and macOS build, with only the two platform layers swapped — which is what makes
cross-compiling it meaningful rather than decorative.
