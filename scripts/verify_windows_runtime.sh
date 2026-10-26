#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Tessera — run the Windows executable and check what it actually does.
#
#  A cross-compiled .exe that passes a PE inspection is still only a hypothesis:
#  the Win32 console layer, the wide-character file APIs and the exception
#  machinery are all exercised for the first time when the image really runs.
#  This script runs it under Wine (a genuine Win32 loader — it catches build and
#  ABI problems such as broken exception tables, not Wine quirks) and compares
#  every deterministic output and exit code against the Linux binary built from
#  the same sources.
#
#  Usage
#    scripts/verify_windows_runtime.sh                     # needs dist/tessera.exe
#    WINE=/path/to/wine scripts/verify_windows_runtime.sh
#    scripts/verify_windows_runtime.sh --keep              # leave the scratch dir
#
#  Exit status is 0 when every check passes and 1 on any failure. When Wine is
#  not installed the script prints SKIPPED and exits 0 — a missing Wine is not a
#  passing verification, and the notice says so in as many words.
#
#  Wine prints FreeType warnings on a headless machine; those lines are filtered
#  from output before comparison, along with timings, which are not reproducible
#  by definition. Nothing else is filtered: an unexpected line fails the check.
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

EXE="${EXE:-dist/tessera.exe}"
LINUX="${LINUX:-build/tessera}"
# Optional large input for the memory-mapped checks; skipped when absent.
BIG="${BIG:-bench/orders10m.csv}"
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1

if [[ ! -f "$EXE" ]]; then
    echo "error: $EXE not found — run scripts/build_windows.sh first" >&2
    exit 2
fi

WINE="${WINE:-$(command -v wine || true)}"
if [[ -z "$WINE" || ! -x "$WINE" ]]; then
    cat <<'MSG'
SKIPPED: Wine is not installed, so the Windows executable has NOT been run.

  The image is still checked structurally by scripts/verify_pe.py, but the Win32
  console layer, wide-character paths and exception handling are only verified by
  executing it. Install Wine (or run the .exe on Windows) and re-run:

      WINE=/path/to/wine scripts/verify_windows_runtime.sh

MSG
    exit 0
fi

export WINEPREFIX="${WINEPREFIX:-$PWD/build/wineprefix}"
export WINEDEBUG="${WINEDEBUG:--all}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tessera-winverify-XXXXXX")"
trap '[[ $KEEP == 1 ]] || rm -rf "$WORK"' EXIT
cp "$EXE" "$WORK/tessera.exe"

PASS=0
FAIL=0
SKIP=0
LINUX_AVAILABLE=0
[[ -x "$LINUX" ]] && LINUX_AVAILABLE=1

# Wine's stderr on a machine without FreeType; harmless and never part of our
# output, but it would otherwise appear in every comparison.
NOISE='FreeType|freetype|TrueType|^use |^or equal|^http://www\.freetype\.org|^wine: |^err: |^fixme:|^warn:'
# Values that legitimately differ between two runs of the same binary.
TIMING='open time|built at open|^  (index|scan|filter|sort) +[0-9]'

normalize() {
    sed -e 's/\r$//' -e 's/\x1b\[[0-9;]*[A-Za-z]//g' |
    grep -Ev "$NOISE" | grep -Ev "$TIMING"
}

note() { printf '  %-6s %s\n' "$1" "$2"; }

# Runs a command in the scratch directory and records both its output (filtered)
# and its exit code. The two are captured separately on purpose: piping straight
# into `normalize` would leave `$?` holding grep's status, which is how an exit
# code check silently becomes a check that grep found nothing to complain about.
CAP_OUT=""
CAP_CODE=0
CAP_RAW=""
capture_win() {
    CAP_RAW="$(cd "$WORK" && "$WINE" tessera.exe "$@" 2>&1)"
    CAP_CODE=$?
    CAP_OUT="$(normalize <<<"$CAP_RAW")"
}
capture_linux() {
    CAP_RAW="$(cd "$WORK" && "$ROOT/$LINUX" "$@" 2>&1)"
    CAP_CODE=$?
    CAP_OUT="$(normalize <<<"$CAP_RAW")"
}

check_contains() {
    local what="$1" needle="$2" haystack="$3"
    if grep -qF -- "$needle" <<<"$haystack"; then
        PASS=$((PASS + 1)); note "ok" "$what"
    else
        FAIL=$((FAIL + 1)); note "FAIL" "$what — output did not contain: $needle"
        sed 's/^/         | /' <<<"$haystack" | head -8
    fi
}

check_exit() {
    local what="$1" expected="$2" actual="$3"
    if [[ "$actual" == "$expected" ]]; then
        PASS=$((PASS + 1)); note "ok" "$what"
    else
        FAIL=$((FAIL + 1)); note "FAIL" "$what — exit $actual, expected $expected"
    fi
}

check_not_contains() {
    local what="$1" needle="$2" haystack="$3"
    if grep -qF -- "$needle" <<<"$haystack"; then
        FAIL=$((FAIL + 1)); note "FAIL" "$what — output unexpectedly contained: $needle"
        sed 's/^/         | /' <<<"$haystack" | head -8
    else
        PASS=$((PASS + 1)); note "ok" "$what"
    fi
}

# The same arguments through both binaries: identical output and identical exit
# code, or the check fails.
check_same() {
    local what="$1"; shift
    if [[ $LINUX_AVAILABLE == 0 ]]; then
        SKIP=$((SKIP + 1)); note "skip" "$what (no Linux binary to compare against)"
        return
    fi
    capture_linux "$@"
    local lx_out="$CAP_OUT" lx_code="$CAP_CODE"
    capture_win "$@"
    if [[ "$lx_out" == "$CAP_OUT" && "$lx_code" == "$CAP_CODE" ]]; then
        PASS=$((PASS + 1)); note "ok" "$what"
    else
        FAIL=$((FAIL + 1)); note "FAIL" "$what — differs from Linux (exit $lx_code vs $CAP_CODE)"
        diff <(printf '%s\n' "$lx_out") <(printf '%s\n' "$CAP_OUT") | sed 's/^/         | /' | head -12
    fi
}

echo "▸ wine       $("$WINE" --version 2>/dev/null)"
echo "▸ image      $EXE"
echo "▸ scratch    $WORK"
echo

# ── Fixtures ─────────────────────────────────────────────────────────────────
cat >"$WORK/basic.csv" <<'CSV'
id,region,units,revenue,created
1,EU,3,1200.50,2024-01-31
2,US,7,980.00,2024-02-01
3,EU,11,2500.25,2024-03-15
4,APAC,5,410.75,2024-04-02
CSV
printf 'id;name;city\n1;Z\xc3\xbcrich;Z\xc3\xbcrich\n2;S\xc3\xa3o Paulo;S\xc3\xa3o Paulo\n3;\xe6\x9d\xb1\xe4\xba\xac;\xe6\x9d\xb1\xe4\xba\xac\n' >"$WORK/données_ventes.csv"
printf 'a,b\n1,2\n' >"$WORK/emoji \xf0\x9f\x9a\x80.csv"
printf '{"id":1,"tag":"a"}\n{"id":2,"tag":"b"}\n' >"$WORK/events.jsonl"

# ── Deterministic one-shot commands ──────────────────────────────────────────
check_same "version"            --version
check_same "info"               info basic.csv
check_same "info --no-color"    info basic.csv --no-color
check_same "head"               head basic.csv -n 2
check_same "tail"               tail basic.csv -n 2
check_same "query group by"     query basic.csv "SELECT region, count(*) AS n, sum(revenue) AS total FROM t GROUP BY region ORDER BY region"
check_same "query where/order"  query basic.csv "SELECT id, units FROM t WHERE units > 3 ORDER BY units DESC"
check_same "query limit"        query basic.csv "SELECT * FROM t" -n 2
check_same "stats"              stats basic.csv
check_same "stats --sample"     stats basic.csv --sample
check_same "export csv"         export basic.csv -f csv -o -
check_same "export jsonl"       export basic.csv -f jsonl -o -
check_same "jsonl info"         info events.jsonl
check_same "semicolon + accents" info "données_ventes.csv"
check_same "emoji filename"     info "emoji 🚀.csv"

# ── Non-ASCII paths must round-trip through the wide-character APIs ──────────
rm -f "$WORK/données_ventes.csv.tessera.idx"
capture_win index "données_ventes.csv"
check_exit "index on an accented path" 0 "$CAP_CODE"
check_contains "index reports the row count" "cached 3 rows" "$CAP_OUT"
if [[ -f "$WORK/données_ventes.csv.tessera.idx" ]]; then
    PASS=$((PASS + 1)); note "ok" "sidecar written with the correct UTF-8 name"
else
    FAIL=$((FAIL + 1)); note "FAIL" "sidecar missing or written under a mangled name"
    ls -1 "$WORK" | sed 's/^/         | /'
fi
capture_win info "données_ventes.csv"
check_contains "the cached index is reused on the next open" "restored from" "$CAP_OUT"

# A cache with the right magic but a corrupt body must be ignored, not trusted.
printf 'TESSIDX4garbage' >"$WORK/données_ventes.csv.tessera.idx"
capture_win info "données_ventes.csv"
check_exit "a corrupt cache is survivable" 0 "$CAP_CODE"
check_contains "a corrupt cache is ignored and rescanned" "rows          3" "$CAP_OUT"
check_not_contains "a corrupt cache is not reported as used" "restored from" "$CAP_OUT"

# A read-only directory cannot take a sidecar; that is an error, not a crash.
mkdir -p "$WORK/rodir" && cp "$WORK/basic.csv" "$WORK/rodir/data.csv" && chmod a-w "$WORK/rodir"
capture_win index "rodir/data.csv"
if [[ "$CAP_CODE" == 0 ]]; then
    SKIP=$((SKIP + 1)); note "skip" "read-only directory (this user can still write anyway)"
else
    check_exit "index in a read-only directory exits non-zero" 1 "$CAP_CODE"
    check_contains "index explains the read-only failure" "read-only" "$CAP_OUT"
fi
chmod u+w "$WORK/rodir" 2>/dev/null || true

# ── Error paths: the exit code and the message, not just "it did not crash" ───
# A build whose exception tables are incomplete prints `libc++abi: terminating`
# and exits 3 here instead of exiting 1 with a sentence. That is the regression
# these cases exist to catch.
error_case() {
    local what="$1" expected_message="$2"; shift 2
    capture_win "$@"
    check_exit "$what exits 1" 1 "$CAP_CODE"
    check_contains "$what explains itself" "$expected_message" "$CAP_OUT"
    check_not_contains "$what does not abort with an uncaught exception" "terminating due to uncaught" "$CAP_OUT"
    check_not_contains "$what does not reach std::terminate" "terminate called" "$CAP_OUT"
}

error_case "missing file"           "no such file"          info missing.csv
error_case "unknown command"        "unknown command"       frobnicate
error_case "unknown output format"  "unknown output format" export basic.csv -f table -o -
error_case "unknown column"         "unknown column"        query basic.csv "SELECT nope FROM t"
error_case "unwritable destination" "cannot create"         export basic.csv -f csv -o /nonexistent-dir/x.csv
error_case "query syntax error"     "query error"           query basic.csv "SELECT FROM WHERE"

# ── Argument errors, thrown while parsing the command line ───────────────────
# These are the paths that used to sit outside the try/catch in `cli::run`, so
# a mistyped flag aborted the process (exit 134 on Linux, 3 on Windows) with a
# C++ runtime message. They are also the most common thing a new user hits, and
# they were missing from the six cases above precisely because the gap was in
# the same frame that caught them.
error_case "unknown flag"           "unknown option"                  --bogusflag basic.csv
error_case "non-numeric row count"  "non-negative whole number"       head basic.csv -n many
error_case "negative row count"      "non-negative whole number"       head basic.csv -n -4
error_case "flag without its value"  "requires a value"                query basic.csv -q
error_case "multi-character delimiter" "--delimiter expects a single character" head basic.csv -d ';;'
error_case "non-numeric thread count" "non-negative whole number"      info basic.csv --threads lots
error_case "bad literal flag pair"  "unknown"                         --no-such-flag basic.csv -n 1

# ── Export to a real file, then compare the file with the Linux export ───────
rm -f "$WORK/out.csv"
capture_win export basic.csv -q "SELECT region, units FROM t WHERE units >= 5 ORDER BY units" -o out.csv
check_exit "export to a file" 0 "$CAP_CODE"
if [[ -f "$WORK/out.csv" ]]; then
    PASS=$((PASS + 1)); note "ok" "exported file exists"
    win_export="$(normalize <"$WORK/out.csv")"
    check_contains "exported file contains a filtered row" "APAC" "$win_export"
    if [[ $LINUX_AVAILABLE == 1 ]]; then
        rm -f "$WORK/out-linux.csv"
        (cd "$WORK" && "$ROOT/$LINUX" export basic.csv -q "SELECT region, units FROM t WHERE units >= 5 ORDER BY units" -o out-linux.csv) >/dev/null 2>&1
        if [[ "$win_export" == "$(normalize <"$WORK/out-linux.csv")" ]]; then
            PASS=$((PASS + 1)); note "ok" "exported file is byte-identical to the Linux export (line endings aside)"
        else
            FAIL=$((FAIL + 1)); note "FAIL" "exported file differs from the Linux export"
            diff <(normalize <"$WORK/out-linux.csv") <(printf '%s\n' "$win_export") | sed 's/^/         | /' | head -10
        fi
    fi
else
    FAIL=$((FAIL + 1)); note "FAIL" "export produced no file"
fi

# ── Piped output must be valid UTF-8 with no escape codes ───────────────────
capture_win info basic.csv
if LC_ALL=C grep -q $'\x1b' <<<"$CAP_RAW"; then
    FAIL=$((FAIL + 1)); note "FAIL" "piped output contains ANSI escape codes"
else
    PASS=$((PASS + 1)); note "ok" "piped output is plain text"
fi
printf '%s' "$CAP_RAW" >"$WORK/piped.txt"
if iconv -f UTF-8 -t UTF-8 "$WORK/piped.txt" >/dev/null 2>&1; then
    PASS=$((PASS + 1)); note "ok" "piped bytes are valid UTF-8"
else
    FAIL=$((FAIL + 1)); note "FAIL" "piped bytes are not valid UTF-8"
fi
if grep -qF "$(printf '\xe2\x97\x86')" "$WORK/piped.txt" && grep -qF "$(printf '\xc2\xb7')" "$WORK/piped.txt"; then
    PASS=$((PASS + 1)); note "ok" "UTF-8 symbols survive the console layer byte for byte"
else
    FAIL=$((FAIL + 1)); note "FAIL" "UTF-8 symbols were mangled in piped output"
    od -c "$WORK/piped.txt" | head -5 | sed 's/^/         | /'
fi

# ── Large file: the memory-mapped path on a real 1.1 GB input ───────────────
# Everything above fits in a few hundred bytes, so it exercises the parser and
# the exception machinery but never the interesting part of the Windows I/O: a
# 1.1 GB `CreateFileMapping` view and a streamed export out of it. The corpus is
# gitignored (scripts/gen_data.py regenerates it byte-for-byte), so this section
# skips loudly rather than pretending it ran.
if [[ ! -f "$BIG" ]]; then
    SKIP=$((SKIP + 1))
    note "skip" "large-file checks (no $BIG — generate it with: python3 scripts/gen_data.py --rows 10000000 --out bench/orders10m.csv)"
elif [[ $LINUX_AVAILABLE == 0 ]]; then
    SKIP=$((SKIP + 1))
    note "skip" "large-file checks (no Linux binary to compare against)"
else
    # The binary runs from the scratch directory, so the corpus must be named by
    # an absolute path — a relative one would silently fail to open in both
    # builds and make the comparison below agree on two identical errors.
    case "$BIG" in /*) ;; *) BIG="$ROOT/$BIG" ;; esac
    BIGQ="SELECT * FROM t WHERE units > 36"
    rm -f "$WORK/big-win.csv" "$WORK/big-linux.csv"
    (cd "$WORK" && "$WINE" tessera.exe export "$BIG" -q "$BIGQ" -o big-win.csv) >/dev/null 2>&1
    win_code=$?
    (cd "$WORK" && "$ROOT/$LINUX" export "$BIG" -q "$BIGQ" -o big-linux.csv) >/dev/null 2>&1
    check_exit "1.1 GB filtered export" 0 "$win_code"
    if [[ -f "$WORK/big-win.csv" && -f "$WORK/big-linux.csv" ]]; then
        win_size=$(wc -c <"$WORK/big-win.csv")
        lx_size=$(wc -c <"$WORK/big-linux.csv")
        if cmp -s "$WORK/big-win.csv" "$WORK/big-linux.csv"; then
            PASS=$((PASS + 1))
            note "ok" "1.1 GB filtered export is byte-identical to the Linux export ($(numfmt --to=iec "$win_size" 2>/dev/null || echo "$win_size B"))"
        else
            FAIL=$((FAIL + 1))
            note "FAIL" "1.1 GB filtered export differs from the Linux export ($win_size B vs $lx_size B)"
        fi
    else
        FAIL=$((FAIL + 1)); note "FAIL" "1.1 GB filtered export produced no file"
    fi

    capture_linux query "$BIG" "SELECT region, count(*) AS n FROM t GROUP BY region ORDER BY region"
    lx_out="$CAP_OUT" lx_code="$CAP_CODE"
    capture_win query "$BIG" "SELECT region, count(*) AS n FROM t GROUP BY region ORDER BY region"
    # Exit 0 is asserted first: without it, two runs that both failed to open the
    # file would compare equal and be reported as agreement.
    check_exit "1.1 GB group-by" 0 "$CAP_CODE"
    if [[ "$lx_out" == "$CAP_OUT" && "$lx_code" == "$CAP_CODE" ]]; then
        PASS=$((PASS + 1)); note "ok" "1.1 GB group-by agrees with the Linux build"
    else
        FAIL=$((FAIL + 1)); note "FAIL" "1.1 GB group-by differs from the Linux build"
        diff <(printf '%s\n' "$lx_out") <(printf '%s\n' "$CAP_OUT") | sed 's/^/         | /' | head -10
    fi
fi

echo
if [[ $FAIL == 0 ]]; then
    echo "▸ $PASS passed, $SKIP skipped, 0 failed — the Windows image runs correctly"
    exit 0
fi
echo "▸ $PASS passed, $SKIP skipped, $FAIL FAILED" >&2
exit 1
