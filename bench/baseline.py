"""Naive in-memory baseline: what a script does today."""
import csv, resource, sys, time
path = sys.argv[1]
rows = int(sys.argv[2]) if len(sys.argv) > 2 else 0
t0 = time.perf_counter()
with open(path, newline="", encoding="utf-8", errors="replace") as f:
    r = csv.reader(f)
    data = [row for row in r]
t1 = time.perf_counter()
rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024.0
print(f"python csv -> list: {len(data):,} rows in {t1-t0:.2f}s, peak RSS {rss/1024:.2f} GB")
