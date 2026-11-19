#!/usr/bin/env bash
# FastNetMon daemon integration + load/stress/soak harness (Phases 3,5,6,7,10 of the checklist).
# Runs the REAL daemon, fed real NetFlow v5 over loopback by netflow_gen.py. Isolated VM only.
#
# Usage:
#   sudo ./run_integration.sh setup          # build-free: configure + install test conf
#   sudo ./run_integration.sh start [THREADS] # start daemon with speed_calculation_threads=THREADS
#   ./run_integration.sh smoke               # 60s: confirm hosts are tracked + workers run
#   ./run_integration.sh load   <hosts> <rate> <secs>
#   ./run_integration.sh stress <hosts> <rate> <secs> <attackers> <attacker_pps>
#   ./run_integration.sh soak   <hosts> <rate> <secs>   # e.g. 10000000 2000000 28800  (8h)
#   sudo ./run_integration.sh stop
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
CONF=/etc/fastnetmon.conf
NETWORKS=/etc/networks_list
LOG=/var/log/fastnetmon.log
BIN="${FNM_BIN:-/opt/fastnetmon-community/app/bin/fastnetmon}"
SUBNET="${SUBNET:-10.0.0.0/8}"
GEN="$(dirname "$0")/netflow_gen.py"
MON="$(dirname "$0")/monitor.sh"

setup() {
  echo "$SUBNET" > "$NETWORKS"
  echo "1.0.0.0/8" > /etc/networks_whitelist 2>/dev/null || true   # keep external peers out of host table
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
  # --- test overrides (real options, from src/fastnetmon.conf) ---
  sed -i \
    -e 's/^netflow = .*/netflow = on/' \
    -e 's/^netflow_host = .*/netflow_host = 127.0.0.1/' \
    -e 's/^enable_ban = .*/enable_ban = on/' \
    -e 's/^process_incoming_traffic = .*/process_incoming_traffic = on/' \
    -e 's/^process_outgoing_traffic = .*/process_outgoing_traffic = on/' \
    "$CONF"
  # detection fires but takes NO external action (no BGP/exabgp/gobgp/api configured); notify just logs:
  cat > /usr/local/bin/notify_about_attack.sh <<'N'
#!/bin/bash
echo "$(date -Is) NOTIFY action=$1 ip=$2 direction=$3 pps=$4" >> /var/log/fnm_attacks.log
N
  chmod +x /usr/local/bin/notify_about_attack.sh
  : > /var/log/fnm_attacks.log
  echo "setup done: networks=$SUBNET, conf=$CONF (netflow on, loopback, ban=detect-only)"
  echo "verify config: $BIN --configuration_check"
}

set_threads() { sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "${1:-16}" >> "$CONF"
  echo "speed_calculation_threads=${1:-16}"; }

start() { set_threads "${1:-16}"; : > "$LOG"
  "$BIN" --configuration_check || { echo "CONFIG INVALID"; exit 1; }
  nohup "$BIN" >> "$LOG" 2>&1 &  echo $! > /tmp/fnm.pid
  sleep 5; if kill -0 "$(cat /tmp/fnm.pid)" 2>/dev/null; then echo "daemon up pid=$(cat /tmp/fnm.pid), log=$LOG"; else echo "DAEMON FAILED TO START — tail $LOG"; tail -20 "$LOG"; fi; }

stop() { [ -f /tmp/fnm.pid ] && kill "$(cat /tmp/fnm.pid)" 2>/dev/null; pkill -x fastnetmon 2>/dev/null; echo "stopped"; }

phase() { # name hosts rate secs [attackers attacker_pps]
  local name=$1 hosts=$2 rate=$3 secs=$4 att=${5:-0} apps=${6:-200000}
  local pid; pid=$(cat /tmp/fnm.pid 2>/dev/null)
  echo "=== PHASE $name: hosts=$hosts rate=$rate flows/s dur=${secs}s attackers=$att ==="
  bash "$MON" "$pid" "$secs" "results_${name}.csv" &  local mpid=$!
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet "$SUBNET" --hosts "$hosts" \
     --rate "$rate" --duration "$secs" --direction both --attackers "$att" --attacker-pps "$apps"
  wait "$mpid" 2>/dev/null
  echo "detections so far: $(wc -l < /var/log/fnm_attacks.log 2>/dev/null || echo 0)"
  echo "errors in log:     $(grep -ciE 'error|fatal|terminate|cannot|failed' "$LOG" 2>/dev/null || echo 0)"
  echo "results -> results_${name}.csv"
}

case "${1:-}" in
  setup) setup ;;
  start) start "${2:-16}" ;;
  smoke) phase smoke 500000 200000 60 ;;
  load)  phase load   "${2:-5000000}"  "${3:-1500000}" "${4:-1800}" ;;
  stress) phase stress "${2:-5000000}" "${3:-3000000}" "${4:-600}" "${5:-50}" "${6:-500000}" ;;
  soak)  phase soak   "${2:-10000000}" "${3:-2000000}" "${4:-28800}" ;;
  stop)  stop ;;
  *) grep '^#' "$0" | sed 's/^# \{0,1\}//' ;;
esac
