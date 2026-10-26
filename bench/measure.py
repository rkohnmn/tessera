import os, sys, time
def run(label, args):
    t0 = time.perf_counter()
    pid = os.fork()
    if pid == 0:
        devnull = os.open(os.devnull, os.O_WRONLY)
        os.dup2(devnull, 1); os.dup2(devnull, 2)
        os.execv(args[0], args)
    _, status, ru = os.wait4(pid, 0)
    t1 = time.perf_counter()
    code = os.waitstatus_to_exitcode(status)
    print(f"{label:32s} {t1-t0:7.3f}s  peakRSS={ru.ru_maxrss/1024.0:7.1f}MB  rc={code}")
T="./build/tessera"
for label, a in [
    ("open+info 10M CSV (first)", ["info","bench/orders10m.csv"]),
    ("open+info 10M CSV (warm)", ["info","bench/orders10m.csv"]),
    ("open+info 4M JSONL", ["info","bench/events4m.jsonl"]),
    ("open+info 2M TSV", ["info","bench/orders2m.tsv"]),
    ("query count(*)", ["query","bench/orders10m.csv","SELECT count(*) n FROM t"]),
    ("query group by region", ["query","bench/orders10m.csv","SELECT region, count(*) n, sum(revenue) rev FROM t GROUP BY region ORDER BY rev DESC"]),
    ("query top-10 by revenue", ["query","bench/orders10m.csv","SELECT order_id, revenue FROM t WHERE status='failed' ORDER BY revenue DESC LIMIT 10"]),
    ("stats 4M JSONL", ["stats","bench/events4m.jsonl"]),
    ("export 1M matching rows", ["export","bench/orders10m.csv","-q","SELECT * WHERE units > 36","-o","bench/out.csv"]),
]:
    run(label, [T]+a)
