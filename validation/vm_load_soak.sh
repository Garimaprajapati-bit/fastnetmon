#!/usr/bin/env bash
# =============================================================================
# FastNetMon 10M — Stages 7 (ingestion), 8 (stress), 9 (soak)
# Runs the REAL daemon at 10M hosts (conntrack ON, 4 workers, ban ON) and drives
# REAL NetFlow ingestion with netflow_gen.py while recalc runs, measuring UDP
# loss, recalc latency under load, CPU, RSS, detection, and leak/drift over time.
#
# Usage:  sudo FNM_SRC=/root/fastnetmon TESTS_DIR=/root/fastnetmon/validation \
#              bash vm_load_soak.sh [ingest|stress|soak|all]
# Env:    WORKERS(4) RATE(150000) SOAK_SECONDS(3600) RSS_GUARD_MB(26000) TUNE_RMEM(1)
#
# Safety: a background guard kills everything if daemon RSS exceeds RSS_GUARD_MB,
# so we never exhaust the 30 GB box. rmem is raised (reversibly) so the collector
# gets a fair receive buffer; originals are restored on exit.
# =============================================================================
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
FNM_BIN="${FNM_BIN:-$FNM_SRC/src/build_own/fastnetmon}"
TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "$0")" && pwd)}"
STAGE="${1:-all}"
WORKERS="${WORKERS:-4}"
CT="${CT:-on}"            # connection tracking on/off for the load stages
RATE="${RATE:-150000}"
SOAK_SECONDS="${SOAK_SECONDS:-3600}"
RSS_GUARD_MB="${RSS_GUARD_MB:-26000}"
TUNE_RMEM="${TUNE_RMEM:-1}"
NETWORKS="${NETWORKS:-10.0.0.0/9 10.128.0.0/11}"   # 10,485,760 hosts
GEN="${GEN:-$FNM_SRC/integration-kit/netflow_gen.py}"; [ -f "$GEN" ] || GEN="$TESTS_DIR/netflow_gen.py"
CONF=/tmp/fnm_load.conf; DLOG=/var/log/fastnetmon.log
TS=$(date +%Y%m%d_%H%M%S); OUT="${OUT:-$PWD/vm_loadsoak_$TS}"; mkdir -p "$OUT"
SUM="$OUT/SUMMARY.txt"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$SUM"; }

# ---- reversible rmem tuning + cleanup ----
OLD_RMEM_MAX=$(sysctl -n net.core.rmem_max 2>/dev/null)
OLD_RMEM_DEF=$(sysctl -n net.core.rmem_default 2>/dev/null)
restore(){ pkill -9 -f netflow_gen.py 2>/dev/null; pkill -9 -x fastnetmon 2>/dev/null
  [ "$TUNE_RMEM" = 1 ] && { sysctl -w net.core.rmem_max=$OLD_RMEM_MAX >/dev/null 2>&1; sysctl -w net.core.rmem_default=$OLD_RMEM_DEF >/dev/null 2>&1; }
  [ -n "${GUARD_PID:-}" ] && kill $GUARD_PID 2>/dev/null; }
trap restore EXIT INT TERM
[ "$TUNE_RMEM" = 1 ] && { sysctl -w net.core.rmem_max=67108864 >/dev/null 2>&1; sysctl -w net.core.rmem_default=33554432 >/dev/null 2>&1; log "rmem raised to 64M (was max=$OLD_RMEM_MAX); will restore on exit"; }

udp_errs(){ awk '/^Udp:/{if(!h){for(i=1;i<=NF;i++)c[$i]=i;h=1;next} print $(c["RcvbufErrors"])+$(c["InErrors"])}' /proc/net/snmp; }

start_daemon(){ # $1=conntrack
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
  printf '%s\n' $NETWORKS > /etc/networks_list; : > /etc/networks_whitelist
  sed -i -e 's#^netflow = .*#netflow = on#' -e 's#^netflow_host = .*#netflow_host = 127.0.0.1#' \
         -e 's#^enable_ban = .*#enable_ban = on#' -e 's#^enable_ban_ipv6 = .*#enable_ban_ipv6 = off#' \
         -e "s#^enable_connection_tracking = .*#enable_connection_tracking = ${1:-on}#" \
         -e 's#^process_incoming_traffic = .*#process_incoming_traffic = on#' \
         -e 's#^process_outgoing_traffic = .*#process_outgoing_traffic = on#' \
         -e 's#^average_calculation_time = .*#average_calculation_time = 5#' "$CONF"
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$WORKERS" >> "$CONF"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1; rm -f "$DLOG"
  FNM_BENCH_LOG=1 nohup "$FNM_BIN" --configuration_file "$CONF" >/tmp/fnm_load_out.txt 2>&1 &
  DPID=$!
  for i in $(seq 1 600); do kill -0 $DPID 2>/dev/null || { log "daemon died at startup"; tail -4 /tmp/fnm_load_out.txt|tee -a "$SUM"; return 1; }
    grep -q "FNM_BENCH recalc_ms" "$DLOG" 2>/dev/null && break; sleep 1; done
  # RSS guard
  ( while kill -0 $DPID 2>/dev/null; do r=$(awk '/VmRSS/{print int($2/1024)}' /proc/$DPID/status 2>/dev/null)
      [ -n "$r" ] && [ "$r" -gt "$RSS_GUARD_MB" ] && { echo "[GUARD] RSS ${r}MB > ${RSS_GUARD_MB}MB — stopping" | tee -a "$SUM"; pkill -9 -f netflow_gen.py; kill -9 $DPID; break; }; sleep 3; done ) &
  GUARD_PID=$!
  log "daemon up (pid=$DPID, workers=$WORKERS, conntrack=${1:-on}), 10.49M hosts allocated"
}

# sample daemon state: recalc min/med over last window + UDP err delta + cpu + rss
sample_window(){ # $1=seconds
  local e0=$(udp_errs); : > /tmp/rc.txt
  local u1 s1 u2 s2; read -r u1 s1 < <(awk '{print $14,$15}' /proc/$DPID/stat 2>/dev/null)
  local n=$(( $1/3 )); for i in $(seq 1 $n); do sleep 3
    grep -oE "FNM_BENCH recalc_ms=[0-9.]+" "$DLOG" | tail -1 | awk -F= '{print $2}' >> /tmp/rc.txt; done
  read -r u2 s2 < <(awk '{print $14,$15}' /proc/$DPID/stat 2>/dev/null)
  local e1=$(udp_errs); local hz=$(getconf CLK_TCK)
  CPUS=$(awk -v a=$u1 -v b=$s1 -v c=$u2 -v d=$s2 -v hz=$hz -v w=$1 'BEGIN{printf "%.2f",((c+d)-(a+b))/hz/w}')
  RSS=$(awk '/VmRSS/{print int($2/1024)}' /proc/$DPID/status 2>/dev/null)
  RCMIN=$(sort -n /tmp/rc.txt|grep .|head -1); RCMED=$(sort -n /tmp/rc.txt|grep .|awk '{a[NR]=$1}END{print a[int((NR+1)/2)]}')
  RCMAX=$(sort -n /tmp/rc.txt|grep .|tail -1); UDPD=$((e1-e0))
  OVERB=$(grep -c "should calculate speed counters in <1 second" "$DLOG" 2>/dev/null)
}

ingest_stage(){
  log "===== Stage 7: REAL INGESTION concurrent with recalc ====="
  start_daemon "$CT" || return 1
  local before=$(udp_errs)
  log "starting generator: 500k distinct hosts in 10.0.0.0/9, rate=$RATE flows/s, 120s, 20 attackers"
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/9 --hosts 500000 --rate "$RATE" \
     --duration 120 --direction both --attackers 20 --attacker-pps 400000 --seed 1 >"$OUT/gen_ingest.txt" 2>&1 &
  GENPID=$!
  sleep 5; sample_window 90
  wait $GENPID 2>/dev/null
  sleep 3
  local dets=$(grep -oE "We have detected attack for [0-9.]+" "$DLOG" | awk '{print $NF}' | sort -u | wc -l)
  log "ingest: recalc_ms under load min=$RCMIN med=$RCMED max=$RCMAX | cpus=$CPUS rss=${RSS}MB | udp_err_delta=$UDPD over_budget=$OVERB | attackers_detected=$dets"
  grep -iE "sent|flows|pps|error" "$OUT/gen_ingest.txt" | tail -4 | tee -a "$SUM"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
}

stress_stage(){
  log "===== Stage 8: STRESS (ramping offered rate) ====="
  start_daemon "$CT" || return 1
  for r in 150000 300000 500000 800000; do
    python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/9 --hosts 500000 --rate "$r" \
       --duration 60 --direction both --attackers 20 --attacker-pps 400000 --seed 1 >"$OUT/gen_stress_$r.txt" 2>&1 &
    GENPID=$!; sleep 5; sample_window 45; wait $GENPID 2>/dev/null
    kill -0 $DPID 2>/dev/null || { log "  daemon stopped during stress at rate=$r (guard or crash)"; break; }
    log "stress rate=$r: recalc_ms min=$RCMIN med=$RCMED max=$RCMAX | cpus=$CPUS rss=${RSS}MB | udp_err_delta=$UDPD over_budget=$OVERB"
  done
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
}

soak_stage(){
  log "===== Stage 9: SOAK (${SOAK_SECONDS}s at 10M + continuous traffic) ====="
  start_daemon "$CT" || return 1
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/9 --hosts 500000 --rate "$RATE" \
     --duration 0 --direction both --attackers 20 --attacker-pps 400000 --seed 1 >"$OUT/gen_soak.txt" 2>&1 &
  GENPID=$!; sleep 10
  local rss0=$(awk '/VmRSS/{print int($2/1024)}' /proc/$DPID/status); local rcfirst="" rclast=""
  echo "elapsed_s,rss_mb,recalc_med_ms,udp_err_total,over_budget" > "$OUT/soak_timeline.csv"
  local t=0
  while [ $t -lt $SOAK_SECONDS ]; do
    kill -0 $DPID 2>/dev/null || { log "  daemon stopped during soak at t=${t}s"; break; }
    sample_window 30; t=$((t+30))
    [ -z "$rcfirst" ] && rcfirst="$RCMED"; rclast="$RCMED"
    echo "$t,$RSS,$RCMED,$(udp_errs),$OVERB" >> "$OUT/soak_timeline.csv"
    log "  soak t=${t}s rss=${RSS}MB recalc_med=${RCMED}ms over_budget=$OVERB"
  done
  pkill -9 -f netflow_gen.py 2>/dev/null
  local rssN=$(awk '/VmRSS/{print int($2/1024)}' /proc/$DPID/status 2>/dev/null)
  log "soak done: RSS ${rss0}MB -> ${rssN}MB (growth $((rssN-rss0))MB) | recalc_med ${rcfirst}ms -> ${rclast}ms | timeline: $OUT/soak_timeline.csv"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
}

log "FNM_SRC=$FNM_SRC OUT=$OUT STAGE=$STAGE WORKERS=$WORKERS RATE=$RATE"
[ -x "$FNM_BIN" ] || { log "ERROR: daemon not at $FNM_BIN"; exit 1; }
[ -f "$GEN" ] || { log "ERROR: netflow_gen.py not found"; exit 1; }
case "$STAGE" in
  ingest) ingest_stage;; stress) stress_stage;; soak) soak_stage;;
  all) ingest_stage; stress_stage; soak_stage; log "STAGES 7-9 DONE -> $OUT";;
  *) log "unknown stage";; esac
