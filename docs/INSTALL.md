# Installing Tessera

Tessera is one static executable. There is no installer, no runtime, no service and no account, so
"installing" means putting a file somewhere on your `PATH`. Every route below produces the same
binary; pick whichever fits your machine.

| Platform | Route | Command |
|---|---|---|
| Windows 10/11 x64 | direct download | download `tessera.exe`, put it anywhere |
| Windows 10/11 x64 | winget | `winget install --id Tessera.Tessera` |
| Windows 10/11 x64 | Scoop | `scoop install tessera` |
| macOS (Apple silicon) | Homebrew | `brew install <owner>/tessera/tessera` |
| macOS (Apple silicon) | direct download | `tar -xzf tessera-macos-arm64.tar.gz` |
| Linux x86_64 | direct download | `tar -xzf tessera-linux-x86_64.tar.gz` |
| Any | from source | `make -j"$(nproc)"` → `build/tessera` |

**The package-manager entries are not live yet.** A manifest is only valid with the SHA-256 of the
artifact it installs, so the ones generated for this release are attached to it and submitted from
there — see [Package manager manifests](#package-manager-manifests). Until those submissions are
merged, `winget install Tessera.Tessera` and `brew install rkohnmn/tessera/tessera` will not
resolve; the direct download and from-source routes below work today.

---

## Windows

### Direct download

Download `tessera.exe` from the [releases page](https://github.com/rkohnmn/tessera/releases),
put it in a directory you own (`%USERPROFILE%\bin` is a reasonable choice for a single file), and add
that directory to `PATH` if you want to call it from anywhere:

```powershell
# optional: make it callable from anywhere, per-user, no admin rights
$dir = "$env:USERPROFILE\bin"
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Move-Item .\tessera.exe $dir -Force
[Environment]::SetEnvironmentVariable("Path", "$env:Path;$dir", "User")
```

Then:

```powershell
tessera.exe orders.csv          # open the explorer
tessera.exe info orders.csv     # one-shot summary, exits
tessera.exe query orders.csv "SELECT region, count(*) FROM t GROUP BY region"
```

The executable is a statically linked `PE32+` image. Its imports are `KERNEL32`, `SHELL32` and the
Universal C Runtime, which ships with Windows 10 and 11 — there is no Visual C++ redistributable to
install and no DLL to keep beside it.

**SmartScreen.** The binary is not code-signed yet, so the first run shows "Windows protected your
PC". *More info* → *Run anyway* proceeds. A signed release removes this warning; until then, prefer
verifying the hash below.

### Uninstall

Delete the file. Nothing else was created — no registry keys, no service, no data directory. The only
thing Tessera ever writes next to your data is an optional `yourfile.csv.tessera.idx` cache, which you
can delete at any time (safe to delete while Tessera is closed; it is rebuilt on demand).

---

## macOS

Apple-silicon builds are provided; Intel macOS is build-from-source only.

```sh
tar -xzf tessera-macos-arm64.tar.gz
sudo install -m 0755 tessera /usr/local/bin/tessera
```

The bundled binary is not notarised, so Gatekeeper quarantines it on first run. Clear the flag
deliberately, having verified the hash:

```sh
xattr -d com.apple.quarantine /usr/local/bin/tessera
tessera --version
```

**Homebrew** — via the project tap (see [package manager manifests](#package-manager-manifests) for
why this needs a submission first):

```sh
brew install rkohnmn/tessera/tessera
brew test rkohnmn/tessera/tessera
```

---

## Linux

```sh
tar -xzf tessera-linux-x86_64.tar.gz
sudo install -m 0755 tessera /usr/local/bin/tessera
tessera --version
```

The Linux binary is linked against the system C++ runtime that ships with every mainstream
distribution from the last several years. If it refuses to start on an older distribution, build from
source — it takes about twenty seconds.

---

## From source

Two equivalent builds. CMake is canonical; the Makefile exists so a fresh checkout is one command.

```sh
# CMake — all platforms, warnings-as-errors
cmake -B build -DCMAKE_BUILD_TYPE=Release -DTESSERA_STRICT=ON -DTESSERA_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Makefile — Linux/macOS convenience
make -j"$(nproc)"
make test
```

Requirements: a C++20 compiler (GCC 13+, Clang 16+, or MSVC 19.3x), CMake 3.20+ for the CMake build,
and nothing else. There are no third-party dependencies — no ICU, no zlib, no ncurses, no SQLite.
That is a deliberate constraint, not an accident of scope: a data tool that has to be compiled
against a dependency tree is a data tool that stops building.

**Cross-compiling the Windows executable from Linux or macOS:**

```sh
curl -fsSL https://ziglang.org/download/0.14.1/zig-linux-x86_64-0.14.1.tar.xz -o zig.tar.xz
tar -xJf zig.tar.xz -C tools && mv tools/zig-linux-x86_64-0.14.1 tools/zig
ZIG=tools/zig/zig scripts/build_windows.sh --strict   # -> dist/tessera.exe
```

**Building on Windows natively** is the recommended path if you want to be certain about the artifact:
`cmake -B build -DTESSERA_STRICT=ON && cmake --build build --config Release` with Visual Studio 2022
produces `build\Release\tessera.exe`, and `ctest --test-dir build -C Release` runs the full suite
against it.

---

## Verifying what you downloaded

Every release publishes `SHA256SUMS.txt`. Check the artifact against it before running it:

```sh
sha256sum --check --ignore-missing SHA256SUMS.txt        # Linux
shasum -a 256 tessera-macos-arm64.tar.gz                 # macOS (compare by eye)
certutil -hashfile tessera.exe SHA256                    # Windows PowerShell also works:
Get-FileHash .\tessera.exe -Algorithm SHA256             #   Get-FileHash
```

You can additionally confirm the Windows image really is a self-contained console executable, with no
unexpected DLL dependencies, using the same script CI runs:

```sh
python3 scripts/verify_pe.py dist/tessera.exe
```

---

## Package manager manifests

A winget or Scoop manifest is only valid with the SHA-256 of the exact artifact it installs, and those
artifacts are built by the release workflow. Manifests are therefore generated **from the published
release archives**, by the release job, and attached to each GitHub release — a manifest generated on a
development machine would carry the hash of a different build and fail on the other side. Nothing
hash-bearing is committed to the repository for that reason.

```sh
# regenerate them locally from a release's own archives
python3 scripts/gen_package_manifests.py --version 1.0.0 \
    --exe dist/tessera.exe \
    --windows-zip tessera-windows-x86_64.zip \
    --linux-tar tessera-linux-x86_64.tar.gz \
    --macos-tar tessera-macos-arm64.tar.gz
```

See [`packaging/README.md`](../packaging/README.md) for the full process. What is *not* yet done is the
submission itself, which is a pull request to a registry this project does not control:

* **winget** — `wingetcreate submit --token <PAT> packaging/generated/winget`, or open a PR against
  [`microsoft/winget-pkgs`](https://github.com/microsoft/winget-pkgs) with the three YAML files.
* **Scoop** — copy `scoop/tessera.json` into a bucket repository, or submit it to
  [`ScoopInstaller/Extras`](https://github.com/ScoopInstaller/Extras). It carries `checkver` and
  `autoupdate`, so later releases need no new manifest.
* **Homebrew** — push `homebrew/tessera.rb` to a tap named `homebrew-tessera`, after which
  `brew install rkohnmn/tessera/tessera` works.

Until each PR is merged, `winget install Tessera.Tessera`, `scoop install tessera` and
`brew install rkohnmn/tessera/tessera` will not resolve, and the table at the top of this page
should be read as "once submitted". The direct download and from-source routes are unaffected.

---

## Troubleshooting

**`tessera: command not found` after a manual copy.** The directory you copied into is not on `PATH`.
On Windows, opening a new terminal after setting the user `Path` variable is required; the current one
keeps the old environment.

**The first open of a huge file pauses for a second or two.** That is the row index being built —
8 bytes per row, single pass, parallel across cores. `tessera index <file>` persists it as a
`.tessera.idx` sidecar next to the data so subsequent opens skip the scan. On network shares, copy the
sidecar along with the data or accept the rebuild.

**`index` fails with "read-only directory".** The sidecar is written next to the data file. If that
directory is read-only, Tessera still reads and queries the file perfectly; only the persistent cache
is unavailable. Point `--cache-index` at a writable location or skip `index` entirely.

**Colours look wrong or box-drawing characters are mangled.** On Windows, Tessera sets the console to
UTF-8 and enables virtual-terminal processing at startup; Windows Terminal and recent `conhost` both
honour this. On a legacy console host, run `chcp 65001` first. Piped output never contains escape
codes, so `tessera query ... > out.csv` is always safe.

**A damaged or stale cache.** Delete the `.tessera.idx` file. The cache is validated against the data
file's size, modification time, header bytes and a checksum; when any of those disagree — or when the
file is truncated by a half-finished download — it is ignored and rebuilt rather than trusted.
