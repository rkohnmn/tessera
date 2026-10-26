# Benchmark harnesses

These scripts produce the numbers in [`../docs/BENCHMARKS.md`](../docs/BENCHMARKS.md). They are small,
dependency-free (Python 3.9+ standard library only) and deliberately independent of each other, so no
single tool can flatter the result.

The corpus itself is **not** committed — a 1 GB CSV does not belong in a git repository, and the
generator is deterministic. Regenerate it first:

```sh
python3 scripts/gen_data.py --rows 10000000 --out bench/orders10m.csv
python3 scripts/gen_data.py --rows 4000000 --format jsonl --out bench/events4m.jsonl
python3 scripts/gen_data.py --rows 2000000 --format tsv --out bench/orders2m.tsv
```

| Script | What it measures |
|---|---|
| `../build/tessera bench <file>` | the tool's own instrumented timing of open, scan, filter and sort, with a checksum so a scan that optimised itself away is visible |
| `measure.py` | wall time and peak RSS **per command**, using `fork` + `wait4` so each child's peak is attributed to that child |
| `measure_anon.sh` | peak **anonymous** (`RssAnon`) vs **file-backed** (`RssFile`) memory, sampled from `/proc/<pid>/status` while the process runs |
| `baseline.py` | the naive Python baseline: `csv.reader` into a list of lists |
| `baseline_pandas.py` | `pandas.read_csv` plus the equivalent `groupby`, with peak RSS sampled from `VmHWM` |

```sh
python3 bench/measure.py
bash bench/measure_anon.sh
python3 bench/baseline.py bench/orders10m.csv
python3 bench/baseline_pandas.py bench/orders10m.csv
```

## Reading the results honestly

* **Peak RSS is not the program's memory.** Memory-mapped file pages count as resident *file-backed*
  memory. That is the OS page cache: it is reclaimable under pressure and will be re-read from disk.
  `measure_anon.sh` exists to separate the two numbers, and `docs/BENCHMARKS.md` reports both.
* **Warm vs cold.** Every figure assumes the file has been read at least once. A cold first read is
  disk-bound and no tool avoids that; `tessera info` on a cold cache is roughly 2–3× the warm figure
  on network-backed storage.
* **Shared hardware.** On a cloud container, ±10% between runs is normal. The ordering of results is
  stable; the third significant digit is not.
* **Single run.** These are not averaged over many iterations. If you need statistical confidence,
  loop the command and take the median — the harnesses are small enough to wrap in a shell loop.
