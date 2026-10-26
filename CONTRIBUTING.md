# Contributing to Tessera

Thanks for considering it. This document is short on ceremony and specific about what keeps the
project coherent.

## Getting set up

You need a C++20 compiler and CMake 3.20+, or just `make` on Linux/macOS. There are no third-party
dependencies to fetch — please keep it that way.

```sh
git clone https://github.com/rkohnmn/tessera && cd tessera

# canonical
cmake -B build-cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
ctest --test-dir build-cmake --output-on-failure

# or the convenience shell
make -j"$(nproc)" && make test
```

Two minutes of setup, one command to test, no generated code, no submodules.

## Before you open a pull request

1. **`make test` passes.** All of it, not most of it.
2. **No new warnings.** CI builds with `-Werror` across GCC, Clang and the Windows cross-compiler.
   Warnings are treated as defects here, not as noise.
3. **The build stays dependency-free.** If your change needs a library, open an issue first and
   argue for it. The zero-dependency property is a feature of the product, not an accident of the
   current code.
4. **New behaviour has a test** in `tests/`, and bug fixes get a test that fails without the fix.
5. **User-facing text is explained, not coded.** Errors are one clear sentence and say what to do.
6. **Benchmarks do not regress.** If you touch the indexer, the scanner, the executor or the
   renderer, run `tessera bench` on a large file before and after and put the numbers in the PR.

## What belongs here

Tessera is deliberately narrow: open a very large delimited or JSON Lines file, show it to a human,
answer questions about it, and get out of the way. Changes that make it *more* of that are welcome.

Things that will be declined, kindly, and usually with a pointer to the right tool:

* Joins across files, subqueries, window functions — that is a database's job, and DuckDB does it
  well. The query language is a frozen, documented subset; see `docs/QUERY.md`.
* Writing to the input file, in any form, behind any flag. Tessera is read-only over your data.
* Telemetry, update checks, crash reporting, or any network access at all.
* A plugin system, a config file format, or a scripting host.
* GUI toolkits and web front ends. The interface is a terminal, and that is a considered choice
  (`docs/DESIGN.md`, decision 6) — the whole value proposition is one small binary.
* A dependency, unless it replaces a large amount of code with something strictly better. Expect to
  be asked what it costs in binary size, build time and portability.

## Code style

Match the surrounding file. The conventions are consistent enough that reading one header will tell
you more than a style guide:

* **C++20, standard library only.** `std::string_view`, `std::optional`, `std::span` where they fit.
* **`snake_case`** for functions and variables, `PascalCase` for types, `kConstant` for constants,
  trailing `_` for private members. Namespaces are `tess`, then `tess::query`, `tess::tui`, `tess::io`.
* **83-column comment banners** at the top of each file explaining what it is for and, where it is
  non-obvious, *why* it is written the way it is. The comments are the documentation of the internals.
* **Comments explain intent, not mechanics.** `// count quotes so we know each chunk's start state`
  is useful; `// increment i` is not.
* **Errors throw `tess::Error`** with a message written for the person who will read it. Nothing in
  the engine prints to stdout or calls `exit`.
* **No platform code outside the two platform layers** — `src/core/source.cpp` for file mapping and
  `src/tui/terminal.cpp` for terminal I/O. Everything else must compile identically everywhere.
* **No mutable globals.** State lives in `Dataset`, `App`, or a job object passed by reference.

`.clang-format` and `.editorconfig` are in the repository; `clang-format -i` on the files you touched
is welcome but not required.

## Testing

`tests/harness.hpp` is a small dependency-free harness. A test is `TEST(name) { CHECK(expr); }` with
`CHECK`, `CHECK_EQ`, `CHECK_NEAR` and `CHECK_THROWS` available; a failure throws, so following
statements in that test do not run.

```sh
make test                                # everything
./build/tessera_tests                    # same thing
./build/tessera_tests profile query      # only tests whose name contains one of these substrings
```

Aim tests at the failures that are **silent** — wrong output that looks plausible — rather than at
the ones that throw visible errors. Quoting, offset arithmetic, UTF-8 widths, timestamp parsing and
aggregate semantics are the areas where a mistake is expensive, and they are correspondingly
well covered. A test that only asserts "it did not crash" earns nothing.

Tests must not depend on the working directory: use `testing::temp_path()` for scratch files,
because ctest runs from the build tree.

### The other four harnesses

Unit tests are examples written to pass, so a change to the reader, the index or the sidecar should
also survive these. CI runs the first two; the last is worth running before you touch the Windows
build.

```sh
scripts/verify_sanitizers.sh                  # ASan + UBSan build, full suite, then fuzz sweep
python3 tests/fuzz_tessera.py --cases 300     # differential vs Python csv + mutation
python3 scripts/gen_edge_cases.py             # the adversarial corpus, as fuzzer-style files
WINE=wine scripts/verify_windows_runtime.sh   # execute the .exe, diff it against the Linux build
```

* **Fuzzing.** A failure is a mismatch with Python's `csv` module, a crash, a hang, or an index that
  is internally impossible. Failing inputs are kept in `artifacts/fuzz-crashes/` with the command to
  replay them; a reported failure is a bug to fix, not a tolerance to widen — the differential
  comparison already allows for the one genuinely ambiguous decision (whether a two-row file has a
  header).
* **The sidecar is untrusted input.** `tests/test_sidecar.cpp` forges files that pass the checksum and
  are still inconsistent — impossible row counts, overflowing deltas, offsets past the end of the
  file, trailing bytes — and requires each to be rejected. When adding a field to the format, bump
  the version in the magic and the constant in that file together.
* **Windows.** A change that touches file handling, the console layer or anything `#if defined(...)`
  should be re-checked with `scripts/verify_windows_runtime.sh`, which catches what a Linux-only test
  suite structurally cannot: narrow-path file APIs and broken exception tables. `-flto` is
  deliberately not used on that target for the second of those reasons — see the comment in
  `scripts/build_windows.sh` before "optimising" it back.

## Benchmarks

If your change is performance-related, the claim needs a measurement, in the PR description:

```sh
python3 scripts/gen_data.py --rows 10000000 --out bench/orders10m.csv
./build/tessera bench bench/orders10m.csv
```

Report the before and after, on the same machine and the same file, and say whether the page cache was
warm. "Feels faster" is not a result; see `docs/BENCHMARKS.md` for how these numbers are produced and
what they do and do not mean.

## Documentation

User-visible behaviour lives in three places, and a change to behaviour should update whichever apply:

* `README.md` — what the program does and why it exists.
* `docs/QUERY.md` — the query language, which is frozen API. Changing it means changing this file and
  saying so in the PR, because people script against it.
* `docs/DESIGN.md` — decisions and the reasoning behind them. If you make a decision that future
  contributors would otherwise re-litigate, add an entry.

## Commit messages and pull requests

Commit subjects in the imperative mood, one line, describing the *why*:

```
Reject a bare column beside an aggregate with no GROUP BY

`select region, count(*) from t` returned the first row's region next to a
whole-file count, which reads as an authoritative answer and is meaningless.
Standard SQL refuses it, so we do too, with a message that names the fix.
```

Keep pull requests focused. One idea per PR is much easier to review and to revert. If a change is
large, say so early in an issue and describe the shape before writing it.

## Reporting bugs

Useful bug reports include: the Tessera version (`tessera version`), the platform, the command you
ran, what you expected, and what happened. For problems with a specific file, a snippet that shows
the shape is enormously useful — especially if it contains a quoting oddity, since that is where CSV
parsing gets interesting. Please do not attach data you are not allowed to share.

## Security

Tessera makes no network connections and writes nothing except files you explicitly ask it to
(`export`, `index`). If you find a way to make it do otherwise — particularly through a malicious
input file, since parsing hostile data is what it does all day — that is a security issue and should
go to the maintainers privately rather than into a public issue.

## Licence

Contributions are accepted under the MIT licence, matching the project. By opening a pull request you
confirm you have the right to submit the work under those terms.
