#!/usr/bin/env bash
# Driver for the 8-core VM run (read-only; standalone benchmark, NOT the daemon).
# Runs the full matrix at 1M and 10M, BOTH shard modes, ONE candidate per process so the
# "# peak VmRSS this process" line is a clean per-candidate measurement. Optionally perf cache/IPC.
#
#   FNM_SRC=/root/fastnetmon  bash run_bench.sh            # full matrix
#   THREADS=1,2,4,8 REP=7 bash run_bench.sh                # override
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
SRC="$(cd "$(dirname "$0")" && pwd)/sweep_bench.cpp"
BIN=/tmp/sweep_bench
THREADS="${THREADS:-1,2,4,8}"; REP="${REP:-7}"; FIXED="${FIXED:-128}"
CANDS="unordered_hash unordered_range flat_hash_map open_addr dense_vec_range"

echo ">> building"; g++ -O2 -std=c++20 -I "$FNM_SRC/src" -I "$FNM_SRC/src/tests" "$SRC" \
  $(pkg-config --cflags --libs absl_flat_hash_map) -o "$BIN" -lpthread || { echo build failed; exit 1; }

for N in 1000000 10000000; do
  for MODE in auto "$FIXED"; do
    echo "==================== N=$N  shardmode=$MODE  (per-candidate process; VmRSS = peak RSS) ===================="
    printf "# %-15s %4s %6s %9s %9s %9s %9s %9s %10s %9s %s   | peakRSS_MB\n" candidate thr parts build_ms sweep_med sweep_min sweep_max Mhost/s est_store_MB lookup_ms full_checksum
    for C in $CANDS; do
      out=$("$BIN" "$N" "$THREADS" "$REP" "$MODE" "$C" 2>&1)
      rss=$(printf '%s\n' "$out" | sed -n 's/.*peak VmRSS this process: \([0-9.]*\) MB.*/\1/p')
      printf '%s\n' "$out" | grep -E "^  $C " | sed "s/\$/   | ${rss:-NA}/"
    done
  done
done

echo "==================== perf cache/IPC (10M, t8, auto) — core counters (uncore absent on VM) ===================="
command -v perf >/dev/null && for C in unordered_hash unordered_range dense_vec_range; do
  echo "-- $C --"
  perf stat -e task-clock,cycles,instructions,cache-references,cache-misses \
     "$BIN" 10000000 8 3 auto "$C" >/dev/null 2>/tmp/ps_$C.txt
  grep -iE "insn per cycle|cache-misses|cache-references|instructions|cycles|task-clock" /tmp/ps_$C.txt
done || echo "perf not installed"
echo ">> done"
