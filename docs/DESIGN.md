# Design decisions

Every entry below is a decision that shaped the program, the alternative that was rejected, and the
reason. They are recorded because the reasoning is the part that is lost first, and because the next
person to work on this deserves to know what was already considered.

---

## 1. C++20, not Rust

The brief for this project allowed Rust "if significantly faster". It is not, for this workload, and
the deciding factors were not performance at all:

* **The dependency graph.** Tessera has no third-party dependencies. Rust's ecosystem pushes toward
  crates for terminal handling, colour, argument parsing, and profiling; each is a supply-chain entry
  and a build-time cost. C++20 with the standard library gave us everything needed
  (`std::string_view`, `std::optional`, `<filesystem>`, `<thread>`, `std::stable_sort`) with zero
  downloads. A tool whose pitch is "one file, no runtime" should not ship a dependency tree, even a
  vendored one.
* **The binary.** A ~1 MB static executable with no runtime to link is the product. Rust can match
  that; it is not easier to achieve.
* **Memory-mapped I/O.** `mmap` and `MapViewOfFile` are one call each, and the memory-mapping story
  is the core of the design. Both languages expose it directly.
* **Zero-cost abstraction is equal.** The hot loops are SWAR over raw bytes, and both compilers
  produce the same code.

Rust's real advantages — borrow-checker safety, `cargo`, fearless concurrency — matter most in large,
concurrent, long-lived services. Tessera is a short-lived, mostly single-writer, read-only process.
C++20 was the right call for the constraints, and the choice is recorded here rather than being
presented as a verdict on the languages in general.

## 2. Memory-map the file instead of reading it

**Decision.** `Source::open_file` maps the file read-only; the engine works on `const char*` + size.

**Alternative rejected.** Read into a `std::vector<char>`. Simple, portable, and it makes every
subsequent access trivially safe.

**Why.** A vector of the file's bytes is the file's size in anonymous memory, which is precisely the
failure mode Tessera exists to avoid — the one that makes pandas need 5.4 GB for our 1.09 GiB test
file. Mapping makes open O(1) in file size and moves the residency question to the kernel, which is
better at it than we are: it can evict clean pages under pressure and re-read them from disk, and it
never double-copies. It also makes the program's own memory ceiling a function of the *index* and the
*result*, which is the honest thing for a tool like this to claim.

**Cost.** Two platform-specific implementations and a lifetime that must outlive every view into it.
Both are contained in `src/core/source.cpp`.

## 3. An 8-byte-per-row index, and no more

**Decision.** One `uint64_t` per physical record, built once at open, cached to a sidecar.

**Alternatives rejected.** (a) No index at all: rescan the file for every seek — unusable for
scrolling to the end of a 40 GB file. (b) A parsed columnar copy: fast queries, but that is a
database, and it brings back the memory problem. (c) Offset + length pairs: 16 bytes per row for a
boundary the next record's offset already implies.

**Why.** 8 bytes per row is the smallest structure that makes random access O(1). Ten million rows
costs 76 MB; a hundred million rows costs 760 MB; a billion rows costs 7.6 GB, which is the point at
which you genuinely need a different tool and `tessera` should say so rather than thrash. Row text
is then decoded lazily from offsets, so memory is proportional to what you *look at*, not to what the
file contains.

**Consequence.** Everything downstream — filtering, sorting, grouping, profiling — is expressed over
*row numbers*, not row data. A filtered view is a `vector<uint64_t>`; a sorted view is the same
vector reordered; an export streams rows one at a time. That single decision is why the memory
figures in the README stay flat as files grow.

## 4. Quote-parity parallelism for the index

**Decision.** Count quote characters per chunk in parallel, use the running parity of the prefix to
know each chunk's starting state, then scan chunk-by-chunk in parallel.

**Alternatives rejected.** (a) Sequential scan: correct, and 1.29 GB/s becomes ~200 MB/s. (b) Split
on a heuristic boundary and repair: this is what most fast CSV parsers do, and it is wrong in exactly
the files that matter — ones with embedded newlines inside quoted fields. (c) A preceding pass to
find only *record* boundaries serially: still serial.

**Why.** Well-formed CSV has an even number of quote characters per record, so parity is a complete
and cheap state summary. The two-pass scheme is lock-free, needs no boundary repair, and is correct
by construction for quoted fields, embedded newlines and doubled quotes. It costs one extra read of
each byte, which memory bandwidth absorbs, and buys near-linear scaling with cores.

**Guard rails.** An odd total quote count means the file is malformed; the engine detects it and
falls back to a repairing sequential scan that abandons a runaway quote after 1 MiB instead of
swallowing the rest of the file. A result that comes out unsorted is sorted and de-duplicated rather
than trusted. Both are belt-and-braces against a heuristic producing a corrupt index.

## 5. SWAR instead of AVX2 intrinsics

**Decision.** Byte search uses word-at-a-time arithmetic in plain C++ (`src/core/simd.hpp`).

**Alternative rejected.** `_mm256_cmpeq_epi8` / `vpcmpgtb` with runtime CPU dispatch.

**Why.** The measured scan rate is 14.89 GB/s against a file being read from page cache, which is
already at the memory-bandwidth ceiling. Intrinsics might buy ~1.5× on the search loop and would cost
five architecture-specific paths, a dispatch table, and portability risk in the single most
correctness-critical function in the program. SWAR compiles to the same instructions on x86-64 where
it matters and still compiles on the CPU you have not thought about yet.

**Revisit when.** Profiling shows byte search above 30% of total runtime on a real workload.

## 6. A diffing cell renderer instead of a TUI toolkit

**Decision.** ~400 lines of `Screen` that keep the previous frame's cells and emit escape sequences
only where characters or attributes changed, batched into runs.

**Alternatives rejected.** ncurses, termbox/termbox2, notcurses, and drawing the whole frame each
tick.

**Why.** Three reasons, in order of importance. (1) **Redraw volume.** A 200-column grid is ~5,000
cells. Emitting all of them per keystroke is megabytes over SSH and produces visible tearing. (2)
**Dependency-free build.** Every TUI toolkit is a dependency with its own build system and portability
folklore; we would rather own 400 lines. (3) **Windows.** ncurses on Windows means PDCurses or a
compatibility layer; the Win32 console API is right there and we needed to touch it for raw input
regardless.

**Cost.** We own the escape-sequence correctness, the resize handling and the input decoder. The
payoff is a grid that scrolls smoothly on a slow link, which is the thing users notice.

## 7. Display width, not byte length

**Decision.** All layout math uses measured display columns (East Asian wide = 2, combining marks = 0,
control bytes = neutralised), not `std::string::size()`.

**Why.** Terminal UIs that get this wrong are visibly broken on any file containing CJK, emoji, or a
stray `\r` from a Windows export, and those are common in the CSV files this tool targets. The fix is
a small amount of code in one place; retrofitting it later means auditing every width calculation.
Column widths, truncation and the record view all measure through the same helpers, so a wide
character cannot desynchronise the grid from its header. These are unit-tested in
`tests/test_text.cpp`.

## 8. A small *frozen* SQL subset, with explanations for what is missing

**Decision.** One table (`t`), `SELECT` / `WHERE` / `GROUP BY` / `ORDER BY` / `LIMIT` / `OFFSET`, a
fixed function list. No joins, no subqueries, no `HAVING`. Unsupported clauses produce a message
that says why and what to do instead.

**Alternatives rejected.** (a) Full SQLite or DuckDB embedded: an excellent product, but then Tessera
is a worse DuckDB with a nicer TUI, and the "one small program" promise dies. (b) A minimal
`--filter`/`--sort` flag set with no expression language: far less useful, and users reinvent a query
language badly in shell. (c) An expanding dialect: the failure mode where a tool becomes unscriptable
because its language drifts.

**Why.** The language is *API*. People will put it in CI scripts and Makefiles. A language that
quietly gains convenient syntax is one you cannot upgrade without breaking users, so the grammar is
declared stable in the parser's header comment and documented in `docs/QUERY.md`. Everything the
language does not support is either a one-line shell pipeline away or genuinely a database's job.

**Semantics chosen on purpose:**

* **Aliases resolve in `WHERE`.** Standard SQL does not allow this, and every human writes it anyway.
  Since we control the parser, we resolve the alias instead of making people repeat a long expression.
* **A bare column beside an aggregate without `GROUP BY` is an error.** The tempting alternative —
  report the first row's value — produces output that looks like an answer and is not one. Silently
  wrong is the worst outcome for a data tool.
* **A column that is grouped-but-not-aggregated reports its group's first row.** This is the
  `country`-depends-on-`region` case, which is exactly how lookup tables are used. It is documented
  rather than accidental.
* **Division by zero is NULL, not an error.** A single bad row should not abort a 10-million-row scan.
* **`NULL` comparisons are false**, as in SQL; `IS NULL` is the way.

## 9. `info`, `stats`, `query`, `head`… as separate verbs

**Decision.** One-shot commands that each do one thing and exit, alongside the interactive explorer.

**Alternative rejected.** Interactive-only, or a single `--mode` flag.

**Why.** The reason people reach for `awk`, `head` and `wc` on a big CSV is that a shell pipeline is a
better interface for a one-off question than a full-screen app. A tool that can only be *looked at*
gets abandoned once the user has 40 files to check. Adding the verbs costs little — they are the same
engine with a different front end — and it means Tessera is scriptable, `xargs`-able and useful in CI
in addition to being pleasant interactively.

## 10. Output adapts to the destination

**Decision.** A TTY gets an aligned, coloured table; a pipe gets clean CSV. Progress bars go to
stderr and only on a TTY.

**Why.** `tessera query f.csv "…" | sort | uniq -c` is a normal thing to want, and escape codes in it
are a bug report. Doing this with one flag (`--no-color`) instead of detection would make the common
case worse, so detection is the default and the flag is an override.

## 11. Sidecar index cache, validated rather than trusted

**Decision.** Optional `.tessera.idx` beside the data file: delta + LEB128 encoded offsets, plus
magic, format version, file size, mtime, an FNV-1a header signature, and an FNV-1a checksum trailing
the payload. Used only when all of them match; written best-effort, never throwing.

**Why.** Re-opening a 1.09 GiB file drops from 1.08 s to 0.17 s, and the same file is usually opened
repeatedly. Validation is what makes a cache safe: a stale index would produce wrong rows, which is
unacceptable, while a *rejected* index costs nothing but the time already spent. Making the write
best-effort means a read-only directory or a file on a network share degrades to "slower" rather than
"broken".

**The sidecar is untrusted input, and treated as such.** It is the one file on disk that gets read
back and believed, so the reader assumes an attacker wrote it and checks everything before allocating
anything:

* The read is bounded (512 MiB) *before* a byte is allocated, so a corrupt or hostile file cannot
  exhaust memory; the file's size is also sanity-checked against the data file's.
* The checksum covers the descriptor fields and every offset, and the write is atomic — a temporary
  file in the same directory plus a rename — so an interrupted run cannot leave a half-written cache
  that a later run mistakes for a complete one.
* The claimed row count is rejected if it exceeds either the payload's length (each offset costs at
  least one byte) or the data file's size (each record is at least one byte), and it is never used to
  pre-allocate: the vector grows while decoding, so a forged count fails the parse instead of
  reserving gigabytes.
* Each delta is compared against `data_size - current` *before* being added, so an offset crafted near
  `UINT64_MAX` cannot wrap the accumulator into a plausible-looking small offset. Offsets must be
  strictly increasing, inside the file, and the payload must end exactly where the header says.

All of that is pinned by `tests/test_sidecar.cpp`, which forges checksum-valid files that are
internally inconsistent — impossible row counts, overflowing deltas, regression offsets, trailing
garbage — and requires each one to be rejected, plus an end-to-end case proving that a corrupted cache
causes a rescan rather than a wrong row count.

**Format versioning.** The magic carries the version (`TESSIDX4`), and an unrecognised version is
rejected outright rather than read optimistically: every layout change so far has changed the meaning
of the offsets, so an old cache is not merely stale, it is wrong. Rebuilding costs a second.

## 12. Errors as copy, not codes

**Decision.** Every user-visible failure is one sentence, with a suggestion when one exists
(`unknown column 'regoin'. Did you mean 'region'?`), echoing the statement and marking the position.

**Why.** The alternative is what CLI tools usually do: `Error: parse error at token 7`. For a tool
whose value is "get to the answer fast", an error the user cannot act on is a bug. Column suggestions
and dialect explanations (`info` prints *why* it chose a delimiter) exist for the same reason.

**The invariant this depends on, and how it was broken.** "One sentence" holds only if every `throw`
is reached from a frame that catches it, which is a property worth testing rather than assuming.
`cli::run` caught `Error` around `dispatch`, but called `parse_options` *before* entering the `try`, so
the most likely failure in the whole program — a mistyped flag — bypassed the handler and reached
`std::terminate`: a C++ runtime message and exit 134 on Linux, exit 3 on Windows, for someone who
typed `--quite` instead of `--quiet`. Seven argument errors behaved that way (unknown flags, a
non-numeric `-n`, a flag with no value, a bad delimiter, a bad thread count), and the suite did not
notice because it tested the engine, not the command line. The fix is moving one statement inside the
`try`; the durable part is the coverage, because the same mistake is one line away from recurring.
Every rejected argument now has a case asserting both the exit code and the sentence, at the unit
level in `tests/test_cli.cpp` and again through the real Windows binary in
`scripts/verify_windows_runtime.sh`. The general rule: a catch block protects exactly the statements
you put inside it, and a failure path with no test has no guarantee.

## 13. Track the display type per column, and stay forgiving

**Decision.** Columns are inferred as text / integer / float / boolean / timestamp from a sample, and
values are converted lazily at the point of use. A numeric column with a few bad cells still compares
numerically; bad cells read as NULL.

**Alternatives rejected.** (a) All text: `units > 36` compares strings, which is wrong and slow. (b)
Strict typing with a load failure: real exports always have a bad cell somewhere.

**Why.** Real files are dirty. The useful behaviour is "compare this as a number if it looks like
one", which is what an analyst does by hand at the point of doubt. `stats` exists to make the
inference auditable rather than mysterious.

## 14. 113 tests, no framework

**Decision.** A ~100-line harness in `tests/harness.hpp` and a suite focused on the places where bugs
are silent.

**Why.** A test framework is a dependency and a build-system integration, and this program's
bug-prone area is not "does the CLI parse `-n`" — it is CSV quoting, offset arithmetic across chunks,
UTF-8 widths and timestamp parsing, where a mistake produces *plausible wrong output* rather than a
crash. Those are the tests that exist. The suite builds with `make test`, `ctest`, or nothing at all
beyond a compiler.

**A correction to that reasoning.** "The bug-prone area is not the CLI" was wrong, and the largest
remaining defect in this release was sitting exactly there: argument parsing outside the `try`, which
aborted the process on a typo (see §12). Not because option parsing is hard, but because the sentence
above — *bugs are silent in the tricky areas* — quietly licensed skipping the boring ones. The engine
is where a mistake is *plausible*; the command line is where a mistake is *certain*, because every
user's first keystrokes go there. `tests/test_cli.cpp` now covers the exit code and the exact message
for each rejected argument.

**The three suites it does not replace.** Unit tests are examples someone wrote to pass, so three
further harnesses attack the same code from outside it. `tests/fuzz_tessera.py` generates random
well-formed files and requires byte-identical agreement with Python's `csv` module after Tessera has
guessed the dialect unaided, then mutates valid files — truncation, byte flips, duplicated quotes,
splices, NULs — and requires malformed input to be *reported* rather than to crash, hang, or produce
an impossible index. `scripts/verify_sanitizers.sh` rebuilds everything under AddressSanitizer and
UBSan with leak detection and runs both suites plus a fuzz sweep, so a one-byte overread is fatal
instead of silent. And `scripts/verify_windows_runtime.sh` runs the shipped `.exe` under Wine and
diffs 87 checks against the Linux build, because the Windows console layer, the wide-character file
APIs and the exception tables are not covered by anything that runs on Linux. Its last two checks
diff a 113 MB filtered export and a full group-by over the 1.1 GB corpus, i.e. the memory-mapped path
on an input too large to fit in any buffer.

**What that bought.** All three real defects found in the final hardening pass came from running the
artifact rather than reasoning about it: `ifstream`/`fopen` on a UTF-8 Windows path (which wrote a
sidecar under a mangled name while reporting success), `-flto` on the `x86_64-windows-gnu` target
producing an image whose exception table was incomplete, and argument parsing outside the `try` in
`cli::run`. The third is the instructive one: it was not platform-specific, it was plainly visible in
the source, and it survived every check until the comparison was widened to include paths that fail
during argument parsing rather than inside the engine. A verification suite is only as good as its
worst-sampled surface, and the remedy for that is never more reading.

## 15. Zig for the Windows cross-build

**Decision.** `scripts/build_windows.sh` uses Zig's bundled clang + MinGW-w64 to produce a static
`PE32+` binary from Linux/macOS. Native Windows builds use CMake with MSVC or MinGW.

**Alternatives rejected.** (a) MSVC in CI only: then the Windows artefact exists only when the runner
cooperates, and cannot be reproduced locally. (b) Shipping no Windows binary: the platform is the
point of the product. (c) A full MSYS2/MinGW install: heavier to set up and to document.

**Why.** The primary artefact of this project is one `.exe`. Being able to produce it from the
development machine, deterministically, with a single command and no licensing, is worth the one
toolchain download — and the file it produces imports only Windows system libraries, which is
checkable and checked.

**No LTO on this target.** The release flags are `-O3 -DNDEBUG`, deliberately without `-flto`.
Link-time optimisation with the MinGW-w64 runtime and libc++ (the runtime Zig ships for
`x86_64-windows-gnu`) produces an image whose exception tables are incomplete: a `throw tess::Error`
from some call sites is not matched by the `catch` in `tessera::cli::run`, so a bad query or an
unreadable path ends in `libc++abi: terminating due to uncaught exception` with exit code 3 instead
of one line on stderr and exit code 1. The failure is invisible until something goes wrong, which is
precisely when a user needs the error message. Dropping LTO costs 60 KB on a 1 MB binary and nothing
measurable in an I/O-bound program. Linux, macOS and MSVC keep LTO, where it is sound and covered by
the suite; the decision is recorded in `scripts/build_windows.sh` next to the flag itself, where the
next person to "optimise the build" will read it.

**Honesty about this.** Cross-compiling proves the code compiles and links for Windows. It does not
prove the binary behaves correctly on Windows; that requires running it there. It is run there —
87 checks under Wine against the same sources built for Linux, and natively on the MSVC CI runner —
but a physical Windows 10/11 machine with a real console host is still the last unknown. See the
limitations section of the README.

## 16. What was deliberately not built

Recorded because restraint is a design decision:

* No joins, no multi-file queries, no cross-file comparison panes.
* No editing or writing to the input file, ever, under any flag.
* No plugin system, no scripting host, no config file format.
* No telemetry, no update check, no network access of any kind.
* No charts beyond the histogram that answers "is this column usable?".
* No Parquet yet — it is columnar, it is a different reader, and it is on the roadmap rather than
  half-implemented.

Every one of these is someone's obvious feature request, and every one of them makes the program
bigger than the promise it is trying to keep.
