#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Tessera — build and test under AddressSanitizer + UndefinedBehaviorSanitizer,
#  then point the fuzzer at the instrumented binary.
#
#  The release build is what ships, but a parser that reads hostile files all day
#  should also be exercised where a one-byte overread is fatal instead of silent.
#  This is the check that turns "it has not crashed yet" into "it cannot read out
#  of bounds on these inputs".
#
#  Usage
#    scripts/verify_sanitizers.sh                 # tests + 60 fuzz cases per mode
#    scripts/verify_sanitizers.sh --cases 400     # wider fuzz sweep
#    scripts/verify_sanitizers.sh --seed 99
#
#  Environment
#    CXX=/path/to/clang++   override the compiler (clang usually reports UBSan
#                           findings with better diagnostics than gcc)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

cd "$(dirname "$0")/.."

CXX="${CXX:-g++}"
CASES=60
SEED=1
BUILD=build-sanitize
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cases)  CASES="$2"; shift 2 ;;
        --seed)   SEED="$2"; shift 2 ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

VERSION="$(sed -n 's/^VERSION *:=[[:space:]]*//p' Makefile | head -1)"
VERSION="${VERSION:-1.0.0}"

echo "▸ compiler   $($CXX --version | head -1)"
echo "▸ sanitizers address,undefined (halt on error, leak check on)"
echo "▸ build dir  $BUILD"

rm -rf "$BUILD"
mkdir -p "$BUILD/obj" "$BUILD/test-obj"

# Compiles every .cpp under `$1` into `$2`, preserving the directory shape, using
# one process per translation unit. xargs propagates a non-zero status when any
# compiler invocation fails, which `set -e` then turns into a hard stop.
compile_tree() {
    local root="$1" out="$2" includes="$3"
    find "$root" -name '*.cpp' -print0 | sort -z | \
    xargs -0 -P "$(nproc)" -I{} sh -c '
        file="$1"; root="$2"; out="$3"; includes="$4"; cxx="$5"; san="$6"; version="$7"
        object="$out/${file#"$root"/}"
        object="${object%.cpp}.o"
        mkdir -p "$(dirname "$object")"
        exec "$cxx" -std=c++20 -g -O1 $san -Isrc $includes -DTESSERA_VERSION="\"$version\"" -c "$file" -o "$object"
    ' _ {} "$root" "$out" "$includes" "$CXX" "$SAN" "$VERSION"
}

echo "▸ compiling library"
compile_tree src "$BUILD/obj" "-Itests"
# main.cpp is only needed by the CLI, not by the test runner.
rm -f "$BUILD/obj/main.o"

echo "▸ compiling tests"
compile_tree tests "$BUILD/test-obj" "-Itests"

echo "▸ linking"
"$CXX" -std=c++20 -g -O1 $SAN -Isrc -DTESSERA_VERSION="\"$VERSION\"" \
    -o "$BUILD/tessera" $(find src -name '*.cpp' | sort) -pthread
"$CXX" -std=c++20 -g -O1 $SAN -o "$BUILD/tessera_tests" \
    $(find "$BUILD/obj" "$BUILD/test-obj" -name '*.o') -pthread

export ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

echo
echo "▸ unit tests under sanitizers"
"$BUILD/tessera_tests"

echo
echo "▸ fuzzing the instrumented CLI ($CASES cases per mode, seed $SEED)"
python3 tests/fuzz_tessera.py --tessera "$BUILD/tessera" --cases "$CASES" --seed "$SEED"

echo
echo "▸ OK — no sanitizer reports, no mismatches, no crashes"
