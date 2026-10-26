#!/usr/bin/env bash
# Samples peak anonymous vs file-backed RSS of a Tessera invocation.
run() {
  local label="$1"; shift
  "$@" >/dev/null 2>&1 &
  local pid=$! anon=0 file=0 a f started=$SECONDS
  while kill -0 "$pid" 2>/dev/null; do
    a=$(awk '/^RssAnon:/{print $2}' /proc/$pid/status 2>/dev/null)
    f=$(awk '/^RssFile:/{print $2}' /proc/$pid/status 2>/dev/null)
    [ -n "$a" ] && [ "$a" -gt "$anon" ] && anon=$a
    [ -n "$f" ] && [ "$f" -gt "$file" ] && file=$f
  done
  wait "$pid"; local rc=$?
  echo "$label|$((SECONDS-started))s|$((anon/1024))|$((file/1024))|$rc"
}
T=./build/tessera
for spec in \
  "info 10M CSV|info|bench/orders10m.csv" \
  "info 4M JSONL|info|bench/events4m.jsonl" \
  "info 2M TSV|info|bench/orders2m.tsv" \
  "count(*) 10M|query|bench/orders10m.csv|SELECT count(*) n FROM t" ; do
  IFS='|' read -r -a parts <<< "$spec"
  run "${parts[0]}" "$T" "${parts[@]:1}"
done
printf '%-18s %8s %10s %10s %4s\n' LABEL TIME  PEAK_ANON  PEAK_MAPPED  RC >/dev/null
