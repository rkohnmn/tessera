import os, sys, time
try:
    import pandas as pd
except Exception as e:
    print("pandas unavailable:", e); sys.exit(3)
path = sys.argv[1]
pid = os.getpid()
import threading
peak = [0]
stop = [False]
def watch():
    while not stop[0]:
        try:
            for line in open(f"/proc/{pid}/status"):
                if line.startswith("VmHWM:"): peak[0] = max(peak[0], int(line.split()[1]))
        except OSError: pass
        time.sleep(0.02)
t = threading.Thread(target=watch, daemon=True); t.start()
t0 = time.perf_counter()
try:
    df = pd.read_csv(path, low_memory=False)
    t1 = time.perf_counter()
    print(f"pandas.read_csv: {len(df):,} rows x {df.shape[1]} cols in {t1-t0:.2f}s, peak RSS {peak[0]/1024/1024:.2f} GB")
    t2 = time.perf_counter()
    g = df.groupby("region")["revenue"].sum().sort_values(ascending=False)
    t3 = time.perf_counter()
    print(f"  groupby region sum: {t3-t2:.2f}s (total {t3-t0:.2f}s)")
    print(g.to_string())
except MemoryError:
    print(f"pandas.read_csv: MemoryError after {time.perf_counter()-t0:.2f}s, peak RSS {peak[0]/1024/1024:.2f} GB")
finally:
    stop[0] = True
