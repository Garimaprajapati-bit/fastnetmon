#!/usr/bin/env bash
# Regression: parallel sweep must NOT change detection. Runs the REAL daemon with the SAME
# deterministic attacker traffic at speed_calculation_threads=1 (OFF, == original) and =4 (ON),
# and compares the set of detected attacker IPs. They must be identical.
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
BIN="${FNM_BIN:-$FNM_SRC/src/build_own/fastnetmon}"
CONF=/etc/fastnetmon.conf; NET=/etc/networks_list; LOG=/var/log/fastnetmon.log
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
GEN="${GEN:-$FNM_SRC/integration-kit/netflow_gen.py}"
[ -f "$GEN" ] || GEN="$SELF_DIR/netflow_gen.py"   # fallback: bundled next to this script
SUBNET=10.0.0.0/16; HOSTS=65536; RATE=120000; DUR=25; ATT=30; APPS=600000; SEED=1

# base config (once)
cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
echo "$SUBNET" > "$NET"; echo "1.0.0.0/8" > /etc/networks_whitelist
sed -i -e 's/^netflow = .*/netflow = on/' -e 's/^netflow_host = .*/netflow_host = 127.0.0.1/' \
       -e 's/^enable_ban = .*/enable_ban = on/' -e 's/^process_incoming_traffic = .*/process_incoming_traffic = on/' \
       -e 's/^process_outgoing_traffic = .*/process_outgoing_traffic = on/' "$CONF"

run_one(){ # $1 = threads
  local TH=$1
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$TH" >> "$CONF"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 2; : > "$LOG"
  nohup "$BIN" >>"$LOG" 2>&1 & local pid=$!
  for i in $(seq 1 60); do kill -0 $pid 2>/dev/null || { echo "daemon died"; tail -5 "$LOG"; return 1; }
    ss -lun 2>/dev/null | grep -q ":2055" && break; sleep 1; done
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet "$SUBNET" --hosts "$HOSTS" --rate "$RATE" \
     --duration "$DUR" --direction both --attackers "$ATT" --attacker-pps "$APPS" --seed "$SEED" >/dev/null 2>&1
  sleep 2
  grep -oE "We have detected attack for [0-9.]+" "$LOG" | awk '{print $NF}' | sort -u > "/tmp/detected_t${TH}.txt"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
  echo "threads=$TH detected_unique=$(wc -l < /tmp/detected_t${TH}.txt)"
}

run_one 1
run_one 4
echo "=== detection diff (empty = identical) ==="
diff /tmp/detected_t1.txt /tmp/detected_t4.txt && echo "IDENTICAL DETECTION -> REGRESSION PASS" || echo "DIFFERENCE -> REGRESSION FAIL"
echo "t1 sample:"; head -5 /tmp/detected_t1.txt
