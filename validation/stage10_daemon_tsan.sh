#!/usr/bin/env bash
# =============================================================================
# Stage 10 (final gate): daemon-level ThreadSanitizer on the conntrack sharding
# under LIVE packet writers + the parallel sweep.
#
# TSan is ~10-20x slower, so this uses a modest host count and light traffic — the
# goal is race DETECTION, not throughput. It runs the real daemon twice under TSan:
#   A) parallel feature ON  (speed_calculation_threads=4, conntrack on)
#   B) feature OFF           (threads=1, conntrack on)
# and compares the race sites. Sites that appear ONLY with the feature ON are the
# ones this work could have introduced; everything common to both is pre-existing
# (gRPC event engine, lock-free global aggregates, etc.) and out of scope.
#
# Usage: sudo FNM_SRC=/root/fastnetmon TESTS_DIR=/root/fastnetmon/validation \
#             bash stage10_daemon_tsan.sh
# =============================================================================
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "$0")" && pwd)}"
GEN="${GEN:-$FNM_SRC/integration-kit/netflow_gen.py}"; [ -f "$GEN" ] || GEN="$TESTS_DIR/netflow_gen.py"
HOSTS_NET="${HOSTS_NET:-10.0.0.0/18}"   # ~16k hosts — enough to exercise shards, sane under TSan
DUR="${DUR:-45}"
BUILD=/root/fastnetmon/src/build_tsan
CONF=/tmp/fnm_tsan.conf; DLOG=/var/log/fastnetmon.log
OUT="${OUT:-$PWD/vm_tsan_$(date +%Y%m%d_%H%M%S)}"; mkdir -p "$OUT"
log(){ echo "[$(date +%H:%M:%S)] $*"; }

# ---- 1. build a TSan daemon (separate dir; keeps the optimized daemon intact) ----
if [ ! -x "$BUILD/fastnetmon" ]; then
  log "building ThreadSanitizer daemon in $BUILD (one-off, a few minutes)..."
  mkdir -p "$BUILD"; ( cd "$BUILD" && cmake "$FNM_SRC/src" -DLINK_WITH_ABSL=ON \
      -DENABLE_MONGODB_SUPPORT=OFF -DENABLE_GOBGP_SUPPORT=ON -DCLICKHOUSE_SUPPORT=OFF -DKAFKA_SUPPORT=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >/"$OUT"/tsan_build.log 2>&1 \
      && cmake --build . --target fastnetmon -j"$(nproc)" >>"$OUT"/tsan_build.log 2>&1 )
fi
[ -x "$BUILD/fastnetmon" ] || { log "TSan build failed — see $OUT/tsan_build.log"; tail -20 "$OUT/tsan_build.log"; exit 1; }
log "TSan daemon ready: $(ls -la $BUILD/fastnetmon | awk '{print $5}') bytes"

run_tsan(){ # $1=threads  -> writes $OUT/tsan_th$1.log, prints unique race sites count
  local TH=$1
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
  printf '%s\n' "$HOSTS_NET" > /etc/networks_list; : > /etc/networks_whitelist
  sed -i -e 's#^netflow = .*#netflow = on#' -e 's#^netflow_host = .*#netflow_host = 127.0.0.1#' \
         -e 's#^enable_ban = .*#enable_ban = on#' -e 's#^enable_ban_ipv6 = .*#enable_ban_ipv6 = off#' \
         -e 's#^enable_connection_tracking = .*#enable_connection_tracking = on#' \
         -e 's#^process_incoming_traffic = .*#process_incoming_traffic = on#' \
         -e 's#^process_outgoing_traffic = .*#process_outgoing_traffic = on#' "$CONF"
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$TH" >> "$CONF"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
  local tlog="$OUT/tsan_th$TH.log"
  TSAN_OPTIONS="halt_on_error=0 history_size=4 second_deadlock_stack=1 log_path=$tlog" \
    nohup "$BUILD/fastnetmon" --configuration_file "$CONF" >/tmp/fnm_tsan_out.txt 2>&1 &
  local pid=$!
  for i in $(seq 1 180); do kill -0 $pid 2>/dev/null || break
    grep -q "Successfully allocated" "$DLOG" 2>/dev/null && break; sleep 1; done
  # drive light traffic to exercise packet-path flow writers concurrently with the sweep
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet "$HOSTS_NET" --hosts 16000 --rate 20000 \
     --duration "$DUR" --direction both --attackers 10 --attacker-pps 300000 --seed 1 >/dev/null 2>&1
  sleep 3; pkill -9 -x fastnetmon 2>/dev/null; sleep 1
  # TSan writes to $tlog.<pid>; gather all
  cat ${tlog}.* > "$tlog" 2>/dev/null || true
  local n=$(grep -c "WARNING: ThreadSanitizer" "$tlog" 2>/dev/null)
  # extract a stable signature per race: the two "Read/Write ... at ... in FUNC" frames
  grep -E "ThreadSanitizer: data race|#0 " "$tlog" 2>/dev/null | grep -oE "in [A-Za-z_][A-Za-z0-9_:<> ]+" | sort -u > "${tlog}.sites"
  echo "$n"
}

log "=== A) feature ON (threads=4, conntrack on) ==="
NON=$(run_tsan 4); log "   TSan warnings (threads=4): $NON ; unique functions: $(wc -l < $OUT/tsan_th4.log.sites 2>/dev/null)"
log "=== B) feature OFF (threads=1, conntrack on) ==="
NOFF=$(run_tsan 1); log "   TSan warnings (threads=1): $NOFF ; unique functions: $(wc -l < $OUT/tsan_th1.log.sites 2>/dev/null)"

log "=== functions appearing ONLY with the parallel feature ON (candidates introduced here) ==="
comm -23 "$OUT/tsan_th4.log.sites" "$OUT/tsan_th1.log.sites" 2>/dev/null | tee "$OUT/only_on.txt"
echo "----"
log "Review $OUT/only_on.txt: any frame in the sharded sweep (process_shard / recalculate_speed / "
log "abstract_subnet_counters) or sharded conntrack (increment_*_flow_counters / sharded_flow_counters) "
log "is a real finding. Frames only in gRPC / telemetry / lock-free global aggregates are pre-existing."
log "Raw logs: $OUT/tsan_th4.log , $OUT/tsan_th1.log"
