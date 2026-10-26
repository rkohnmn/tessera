# Packaging

Tessera ships as one file, so "packaging" means telling the package managers where that file is.
Three channels are set up here: **winget**, **Scoop** and a **Homebrew tap**.

## Why there are no manifests committed in this directory

A winget or Scoop manifest is not valid without the SHA-256 of the exact artifact it installs. Those
artifacts are produced by the release workflow, so a manifest generated on a development machine
describes a *different* file with a *different* hash — it would look correct, install nothing, and
fail verification on the other side. Exchanging one honest gap for a silent wrong answer is not a good
trade, so the manifests are generated per release from the assets that release actually publishes.

`packaging/generated/` (git-ignored) is where that happens. Its contents are attached to each GitHub
release, so the files to submit are always the ones whose hashes match the download.

## Generating them

```sh
python3 scripts/gen_package_manifests.py --version 1.0.0 \
    --windows-zip tessera-windows-x86_64.zip \
    --linux-tar   tessera-linux-x86_64.tar.gz \
    --macos-tar   tessera-macos-arm64.tar.gz \
    --exe         dist/tessera.exe
```

Output, in `packaging/generated/`:

| File | Channel | Notes |
|---|---|---|
| `winget/Tessera.Tessera.yaml` | winget | version manifest |
| `winget/Tessera.Tessera.locale.en-US.yaml` | winget | description, tags, links |
| `winget/Tessera.Tessera.installer.yaml` | winget | installs the zip, exposes `tessera` |
| `scoop/tessera.json` | Scoop | `bin: tessera.exe`, `checkver`/`autoupdate` wired to GitHub releases |
| `homebrew/tessera.rb` | Homebrew | Apple-silicon tarball, with a `brew test` |

Each manifest is generated from the archive it points at, so the hash in the file is the hash of the
file users download. The script refuses to invent one: with no archive present it prints a note and
skips that manifest rather than writing a placeholder.

## Submitting, once a release exists

Two of the three need a pull request against a registry this project does not own, so they are the
only steps that cannot be automated from inside the repository.

**winget**

```sh
winget install wingetcreate
wingetcreate submit --token <PAT> packaging/generated/winget
```

That opens a pull request against [`microsoft/winget-pkgs`](https://github.com/microsoft/winget-pkgs)
for review. To submit by hand, the three YAML files go in
`manifests/t/Tessera/Tessera/<version>/`.

**Scoop** — the manifest is self-updating via `checkver`/`autoupdate`, so it only needs to exist once
in a bucket:

```sh
git clone https://github.com/<owner>/scoop-bucket && cd scoop-bucket
mkdir -p bucket && cp ../packaging/generated/scoop/tessera.json bucket/
git add bucket/tessera.json && git commit -m "tessera 1.0.0" && git push
```

or submit it to [`ScoopInstaller/Extras`](https://github.com/ScoopInstaller/Extras).

**Homebrew** — push `homebrew/tessera.rb` to a tap repository named `homebrew-tessera`, then:

```sh
brew install <owner>/tessera/tessera
brew test <owner>/tessera/tessera
```

## What a package manager cannot fix

SmartScreen and Gatekeeper warnings come from the binary being unsigned, not from how it is
distributed: `winget install` does not change that, because the check happens when the downloaded
executable runs. Code signing is the only fix; `docs/INSTALL.md` tells users what the warning looks
like and how to verify the hash instead.
