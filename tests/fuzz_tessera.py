#!/usr/bin/env python3
"""
Fuzz Tessera's reader against Python's csv module, and its parser against
adversarial bytes.

Two modes, both seeded so a failure is reproducible:

  differential  Generate random but *well-formed* delimited files, then check
                that Tessera sees exactly what Python's csv module sees — same
                row count, same field values — after detection has guessed the
                dialect on its own. Serialisation uses Python's csv writer, so
                a bug in either reader shows up as a mismatch.

  mutation      Take valid files and corrupt them: truncate, flip bytes,
                duplicate quotes, splice records, inject NULs. A malformed file
                may be reported as an error, but it must never crash, hang, or
                produce an impossible index (rows out of order, a negative
                count, a cell that reads past the buffer).

    python3 tests/fuzz_tessera.py --cases 300
    python3 tests/fuzz_tessera.py --mode mutation --cases 500 --seed 7
    python3 tests/fuzz_tessera.py --tessera build/tessera --keep

Exit status is 0 when every case passes. On failure the offending input is
written to artifacts/fuzz-crashes/ (a gitignored scratch directory, so a fuzz
run never litters the repository root) and the reproducing command is printed.
"""
from __future__ import annotations

import argparse
import csv
import io
import os
import random
import shutil
import string
import subprocess
import sys
import tempfile

REPRO_DIR = os.path.join(os.getcwd(), "artifacts", "fuzz-crashes")


def repro_path(name: str) -> str:
    """Where a failing case is preserved so it can be replayed by hand."""
    os.makedirs(REPRO_DIR, exist_ok=True)
    return os.path.join(REPRO_DIR, name)


TOKENS = [
    "", "a", "alpha", "Zürich", "東京", "🚀", "a,b", 'say "hi"', "line\nbreak",
    "crlf\r\nbreak", "  padded  ", "007", "-12", "3.5", "1e6", "NULL", "0",
    'quote"mid', "tab\there", "semi;colon", "pipe|char", "x" * 40,
]


def random_cell(rng: random.Random) -> str:
    if rng.random() < 0.55:
        return rng.choice(TOKENS)
    length = rng.randint(0, 12)
    alphabet = string.ascii_letters + string.digits + " ,;|\t\"'\n-_."
    return "".join(rng.choice(alphabet) for _ in range(length))


def write_case(path: str, rng: random.Random) -> tuple[list[list[str]], bool, str]:
    """Writes a random delimited file. Returns (rows, has_header, delimiter)."""
    delimiter = rng.choice([",", ";", "\t", "|"])
    quote = '"'
    lineterminator = rng.choice(["\n", "\r\n"])
    columns = rng.randint(1, 6)
    rows = rng.randint(0, 18)
    has_header = rng.random() < 0.7

    table: list[list[str]] = []
    if has_header:
        table.append([f"col{i}" for i in range(columns)])
    for _ in range(rows):
        table.append([random_cell(rng) for _ in range(columns)])

    buffer = io.StringIO()
    writer = csv.writer(buffer, delimiter=delimiter, quotechar=quote,
                        lineterminator=lineterminator, quoting=csv.QUOTE_MINIMAL)
    for record in table:
        writer.writerow(record)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(buffer.getvalue())
    return table, has_header, delimiter


def parse_reference(path: str, delimiter: str) -> list[list[str]]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.reader(handle, delimiter=delimiter, quotechar='"'))


def run(tessera: str, args: list[str], timeout: float = 120.0) -> subprocess.CompletedProcess:
    return subprocess.run([tessera, *args], capture_output=True, timeout=timeout)


def count_rows(tessera: str, path: str) -> int:
    result = run(tessera, ["query", path, "SELECT count(*) n FROM t"])
    if result.returncode != 0:
        raise AssertionError(f"query failed: {result.stderr.decode('utf-8', 'replace').strip()}")
    lines = result.stdout.decode("utf-8", "replace").strip().splitlines()
    return int(lines[-1].split(",")[-1])


def exported_rows(tessera: str, path: str, out: str) -> list[list[str]]:
    result = run(tessera, ["export", path, "-o", out])
    if result.returncode != 0:
        raise AssertionError(f"export failed: {result.stderr.decode('utf-8', 'replace').strip()}")
    with open(out, "r", encoding="utf-8", newline="") as handle:
        return list(csv.reader(handle))


def differential(tessera: str, cases: int, seed: int, workdir: str, keep: bool) -> int:
    failures = 0
    for case in range(cases):
        rng = random.Random(seed + case)
        path = os.path.join(workdir, f"case{case}.csv")
        table, has_header, delimiter = write_case(path, rng)
        try:
            parsed = parse_reference(path, delimiter)
            # Whether the first record is a header is genuinely ambiguous on
            # tiny files, and guessing it is a heuristic, not a correctness
            # property. So accept either interpretation — but only one of them,
            # and only if the *content* matches exactly. Dropping or merging a
            # record is always a failure.
            got = count_rows(tessera, path)
            out = os.path.join(workdir, f"case{case}.out.csv")
            produced = exported_rows(tessera, path, out)
            produced_data = produced[1:] if produced else []

            if produced_data == parsed:
                interpretation = "no header"
            elif produced_data == parsed[1:]:
                interpretation = "header"
            else:
                for i, (want, have) in enumerate(zip(parsed, produced_data)):
                    if want != have:
                        raise AssertionError(f"row {i}: expected {want!r}, got {have!r}")
                raise AssertionError(
                    f"row count: expected {len(parsed)} (or {len(parsed) - 1} with a header), "
                    f"got {len(produced_data)}"
                )

            if got != len(produced_data):
                raise AssertionError(
                    f"count(*) disagrees with export: {got} vs {len(produced_data)} rows"
                )
            del interpretation, has_header
        except AssertionError as error:
            failures += 1
            repro = repro_path(f"failure-{seed + case}.csv")
            shutil.copy(path, repro)
            print(f"  FAIL case {case} (seed {seed + case}): {error}")
            print(f"       saved: {repro}")
            print(f"       try:   {tessera} info {repro}")
        else:
            if not keep:
                os.unlink(path)
    return failures


def mutate(rng: random.Random, data: bytes) -> bytes:
    choice = rng.random()
    if not data:
        return data
    if choice < 0.25:  # truncate
        return data[: rng.randint(0, len(data))]
    if choice < 0.45:  # flip a byte
        position = rng.randrange(len(data))
        return data[:position] + bytes([rng.randrange(32, 127)]) + data[position + 1 :]
    if choice < 0.6:  # duplicate a quote
        position = rng.randrange(len(data))
        return data[:position] + b'"' + data[position:]
    if choice < 0.75:  # splice a chunk over itself
        start = rng.randrange(len(data))
        length = rng.randint(1, max(1, len(data) // 4))
        return data[:start] + data[start : start + length] + data[start:]
    if choice < 0.9:  # inject control bytes
        position = rng.randrange(len(data))
        return data[:position] + bytes([rng.choice([0, 10, 13, 26, 0xFF])]) + data[position:]
    return data + bytes([rng.randrange(256)])


def mutation(tessera: str, cases: int, seed: int, workdir: str, keep: bool) -> int:
    base_sources = [
        b"id,name,amount\n1,alpha,10\n2,beta,20\n3,gamma,30\n",
        b'"a","b"\n"x,1","y\n2"\n"z","w"\n',
        b"id;name\n1;Z\xc3\xbcrich\n2;S\xc3\xa3o Paulo\n",
        b'{"a":1}\n{"a":2,"b":3}\n',
    ]
    failures = 0
    for case in range(cases):
        rng = random.Random(seed * 1000 + case)
        data = rng.choice(base_sources)
        for _ in range(rng.randint(1, 4)):
            data = mutate(rng, data)
        path = os.path.join(workdir, f"mut{case}.csv")
        with open(path, "wb") as handle:
            handle.write(data)
        try:
            result = run(tessera, ["info", path], timeout=60.0)
            if result.returncode not in (0, 1):
                raise AssertionError(
                    f"unexpected exit {result.returncode} (likely a crash); stderr="
                    f"{result.stderr.decode('utf-8', 'replace')[:120]!r}"
                )
            if result.returncode == 0:
                # Whatever it decided to see, the index must be self-consistent.
                counted = count_rows(tessera, path)
                if counted < 0:
                    raise AssertionError("negative row count")
                head = run(tessera, ["head", path, "-n", "3"], timeout=60.0)
                if head.returncode != 0:
                    raise AssertionError(f"head failed after a successful info: {head.stderr[:120]!r}")
        except subprocess.TimeoutExpired:
            failures += 1
            repro = repro_path(f"hang-{seed}-{case}.csv")
            shutil.copy(path, repro)
            print(f"  FAIL case {case}: timeout (possible hang); saved {repro}")
        except AssertionError as error:
            failures += 1
            repro = repro_path(f"mutation-{seed}-{case}.csv")
            shutil.copy(path, repro)
            print(f"  FAIL case {case} (seed {seed * 1000 + case}): {error}")
            print(f"       saved: {repro}")
        else:
            if not keep:
                os.unlink(path)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tessera", default="./build/tessera", help="binary to fuzz")
    parser.add_argument("--cases", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--mode", choices=["differential", "mutation", "both"], default="both")
    parser.add_argument("--keep", action="store_true", help="keep every generated file")
    args = parser.parse_args()

    if not os.path.exists(args.tessera):
        print(f"error: {args.tessera} not found; build first (make -j)", file=sys.stderr)
        return 2

    workdir = tempfile.mkdtemp(prefix="tessera-fuzz-")
    failures = 0
    try:
        if args.mode in ("differential", "both"):
            print(f"differential: {args.cases} cases against Python's csv module")
            failures += differential(args.tessera, args.cases, args.seed, workdir, args.keep)
        if args.mode in ("mutation", "both"):
            print(f"mutation: {args.cases} corrupted inputs")
            failures += mutation(args.tessera, args.cases, args.seed, workdir, args.keep)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print()
    if failures:
        print(f"FAILED: {failures} case(s)")
        return 1
    print("OK: no mismatches, no crashes, no hangs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
