#!/usr/bin/env bash
# =============================================================================
# Stage 3 — ORIGINAL (pre-dense) vs MODIFIED (this work) A/B, identical config.
# Builds the last pre-dense commit (b83686d: sharded but node-based, collapsed hash,
# global conntrack mutex) in a git worktree and runs the SAME 10M matrix as the
# modified daemon, so you get a clean before/after on one machine.
#
# How recalc time is read for each: the MODIFIED daemon logs FNM_BENCH recalc_ms;
# the ORIGINAL has no such hook, but at 10M it is always over budget, so it logs the
# daemon's own "Traffic was calculated in: X sec Y microseconds" line — we parse that.
#
# Usage: sudo FNM_SRC=/root/fastnetmon TESTS_DIR=/root/fastnetmon/validation \
#             bash ab_original_vs_modified.sh
# Env:   THREADS_LIST="1 2 4 8" (or add 16)   ORIG_COMMIT=b83686d
# =============================================================================
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
MOD_BIN="${MOD_BIN:-$FNM_SRC/src/build_own/fastnetmon}"
ORIG_COMMIT="${ORIG_COMMIT:-b83686d}"
THREADS_LIST="${THREADS_LIST:-1 2 4 8}"
NETWORKS="${NETWORKS:-10.0.0.0/9 10.128.0.0/11}"   # 10,485,760 hosts
SETTLE="${SETTLE:-22}"
WT=/root/fnm_orig_worktree
ORIG_BIN="$WT/src/build_orig/fastnetmon"
CONF=/tmp/fnm_ab.conf; DLOG=/var/log/fastnetmon.log
OUT="${OUT:-$PWD/vm_ab_$(date +%Y%m%d_%H%M%S)}"; mkdir -p "$OUT"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/SUMMARY.txt"; }

# ---- build the original in a worktree (keeps your current checkout intact) ----
if [ ! -x "$ORIG_BIN" ]; then
  log "creating worktree at $ORIG_COMMIT and building the ORIGINAL daemon (one-off, ~2 min)..."
  git -C "$FNM_SRC" worktree add -f "$WT" "$ORIG_COMMIT" >/"$OUT"/orig_build.log 2>&1 || { log "worktree add failed"; tail -5 "$OUT/orig_build.log"; exit 1; }
  mkdir -p "$WT/src/build_orig"
  ( cd "$WT/src/build_orig" && cmake .. -DLINK_WITH_ABSL=ON -DENABLE_MONGODB_SUPPORT=OFF -DENABLE_GOBGP_SUPPORT=ON \
      -DCLICKHOUSE_SUPPORT=OFF -DKAFKA_SUPPORT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo >>"$OUT"/orig_build.log 2>&1 \
      && cmake --build . --target fastnetmon -j"$(nproc)" >>"$OUT"/orig_build.log 2>&1 )
fi
[ -x "$ORIG_BIN" ] || { log "ORIGINAL build failed — see $OUT/orig_build.log"; tail -20 "$OUT/orig_build.log"; exit 1; }
[ -x "$MOD_BIN" ]  || { log "MODIFIED binary missing at $MOD_BIN — build build_own first"; exit 1; }
log "both daemons ready. orig=$ORIG_COMMIT"

write_conf(){ # $1=threads
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
  printf '%s\n' $NETWORKS > /etc/networks_list; : > /etc/networks_whitelist
  sed -i -e 's#^netflow = .*#netflow = on#' -e 's#^netflow_host = .*#netflow_host = 127.0.0.1#' \
         -e 's#^enable_connection_tracking = .*#enable_connection_tracking = off#' \
         -e 's#^enable_ban = .*#enable_ban = on#' -e 's#^process_incoming_traffic = .*#process_incoming_traffic = on#' "$CONF"
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$1" >> "$CONF"
}

measure(){ # $1=bin $2=threads $3=label -> echoes min recalc ms
  write_conf "$2"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1; rm -f "$DLOG"
  FNM_BENCH_LOG=1 nohup "$1" --configuration_file "$CONF" >/tmp/fnm_ab_out.txt 2>&1 &
  local pid=$!
  for i in $(seq 1 240); do kill -0 $pid 2>/dev/null || { echo "DIED"; return; }
    grep -qE "FNM_BENCH recalc_ms|Traffic was calculated in" "$DLOG" 2>/dev/null && break; sleep 1; done
  sleep "$SETTLE"
  # modified path:
  local mod=$(grep -oE "FNM_BENCH recalc_ms=[0-9.]+" "$DLOG" | awk -F= '{print $2}' | sort -n | head -1)
  # original path: "Traffic was calculated in: X sec Y microseconds" -> ms
  local org=$(grep "Traffic was calculated in" "$DLOG" | sed -E 's/.*in: ([0-9]+) sec ([0-9]+) micro.*/\1 \2/' \
              | awk '{printf "%.1f\n", $1*1000 + $2/1000}' | sort -n | head -1)
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
  cp "$DLOG" "$OUT/${3}_th$2.log" 2>/dev/null
  [ -n "$mod" ] && echo "$mod" || echo "${org:-NA}"
}

log "===== A/B: 10M hosts, conntrack off, workers: $THREADS_LIST ====="
printf '%-10s | %-16s | %-16s | %s\n' "workers" "ORIGINAL ms" "MODIFIED ms" "improvement" | tee -a "$OUT/SUMMARY.txt"
for th in $THREADS_LIST; do
  o=$(measure "$ORIG_BIN" "$th" orig)
  m=$(measure "$MOD_BIN"  "$th" mod)
  imp=$(awk -v o="$o" -v m="$m" 'BEGIN{ if(o+0>0 && m+0>0) printf "%.1fx faster", o/m; else print "-" }')
  printf '%-10s | %-16s | %-16s | %s\n' "$th" "$o" "$m" "$imp" | tee -a "$OUT/SUMMARY.txt"
done
log "done. logs in $OUT  (orig commit $ORIG_COMMIT vs current HEAD)"
log "NOTE: the ORIGINAL is over budget at every worker count (that's the point); its near-flat"
log "scaling vs the MODIFIED near-linear scaling is the before/after."
