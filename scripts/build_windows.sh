#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Tessera — cross-compile the Windows executable from Linux or macOS.
#
#  Zig ships a full clang toolchain plus a bundled MinGW-w64 C++ runtime, so it
#  can produce a statically linked, dependency-free tessera.exe without a
#  Windows machine or an MSVC licence. We use it only as the *cross* compiler:
#  on Windows itself, MSVC or MinGW via CMake is the supported path.
#
#  Usage
#    scripts/build_windows.sh                 # release build  -> dist/tessera.exe
#    scripts/build_windows.sh --debug         # -O0 -g          -> dist/tessera-debug.exe
#    scripts/build_windows.sh --strict        # warnings as errors
#
#  Environment
#    ZIG=/path/to/zig   override toolchain discovery
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

cd "$(dirname "$0")/.."
VERSION="$(sed -n 's/^VERSION *:=[[:space:]]*//p' Makefile | head -1)"
VERSION="${VERSION:-1.0.0}"

MODE=release
STRICT=0
for arg in "$@"; do
    case "$arg" in
        --debug)  MODE=debug ;;
        --strict) STRICT=1 ;;
        -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

find_zig() {
    if [[ -n "${ZIG:-}" ]]; then echo "$ZIG"; return; fi
    if command -v zig >/dev/null 2>&1; then command -v zig; return; fi
    # Tools vendored next to the checkout (used by CI caches and this repo).
    for candidate in tools/zig-*/zig .tools/zig-*/zig; do
        [[ -x "$candidate" ]] && { echo "$candidate"; return; }
    done
    echo ""
}

ZIG_BIN="$(find_zig)"
if [[ -z "$ZIG_BIN" ]]; then
    cat >&2 <<'MSG'
error: no Zig toolchain found.

Install one of:
  * apt/dnf/pacman package `zig`
  * a release tarball from https://ziglang.org/download  (zig 0.13 or newer)
then re-run, or point ZIG=/path/to/zig at it.

The result is a self-contained PE32+ executable that runs on 64-bit Windows 10+
with no Visual C++ redistributable and no DLLs alongside it.
MSG
    exit 1
fi

echo "▸ toolchain  $("$ZIG_BIN" version)  ($ZIG_BIN)"
echo "▸ target     x86_64-windows-gnu   static runtime"
echo "▸ version    $VERSION"

OUT_DIR=dist
mkdir -p "$OUT_DIR" build/windows-obj

# NOTE on -flto: deliberately NOT used for x86_64-windows-gnu. Link-time
# optimisation on that target (MinGW-w64 runtime + libc++, as shipped with Zig)
# produces a binary whose exception tables are incomplete: `throw tess::Error`
# from some call sites is not caught by the handler in main, so a bad query or an
# unreadable path ends in `libc++abi: terminating due to uncaught exception` and
# exit code 3 instead of a one-line error and exit code 1. The failure is
# invisible in a normal run — the program works — and appears only on the error
# paths, which is exactly where a user needs it to work. Verified by building the
# same sources with and without -flto and running every error path; see
# scripts/verify_windows_runtime.sh, which checks the exit codes.
#
# The Linux/macOS builds keep LTO (CMake's TESSERA_LTO), where it is verified by
# the test suite, and the native MSVC build keeps it where MSVC's LTO is sound.
if [[ "$MODE" == debug ]]; then
    OPT=(-O0 -g)
    EXE="$OUT_DIR/tessera-debug.exe"
else
    OPT=(-O3 -DNDEBUG)
    EXE="$OUT_DIR/tessera.exe"
fi

WARN=(-Wall -Wextra -Wpedantic -Wshadow -Wcast-align -Wdouble-promotion)
[[ "$STRICT" == 1 ]] && WARN+=(-Werror)

mapfile -t SOURCES < <(find src -name '*.cpp' | sort)
echo "▸ compiling  ${#SOURCES[@]} translation units"

# -municode selects the wmain() entry point, which is what main.cpp implements
# so that non-ASCII paths survive on the Windows command line.
"$ZIG_BIN" c++ \
    -target x86_64-windows-gnu -municode -std=c++20 \
    "${OPT[@]}" "${WARN[@]}" \
    -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
    -DTESSERA_WINDOWS=1 -DTESSERA_VERSION="\"$VERSION\"" \
    -Isrc \
    "${SOURCES[@]}" \
    -o "$EXE" \
    -static \
    -Wl,-s -Wl,--gc-sections \
    -lshell32 -ladvapi32 -luser32 -lole32

ls -l "$EXE"
file "$EXE" 2>/dev/null || true
echo "▸ done — copy $EXE to any 64-bit Windows machine and run it."
