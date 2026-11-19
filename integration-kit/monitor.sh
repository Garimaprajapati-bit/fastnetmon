#!/usr/bin/env bash
# Time-series monitor for a running fastnetmon during a test phase.
# Samples every 5s to CSV: RSS, CPU%, thread count, system load, UDP input drops,
# cumulative attack detections, cumulative log errors. Flags crash (process gone).
#   monitor.sh <pid> <seconds> <out.csv>
set -u
PID=${1:?pid}; SECS=${2:-600}; OUT=${3:-monitor.csv}
LOG=/var/log/fastnetmon.log; ATK=/var/log/fnm_attacks.log
echo "elapsed_s,rss_mb,cpu_pct,threads,load1,udp_rcvbuf_errors,detections,log_errors,alive" > "$OUT"
udp_drops() { awk '/Udp:/{getline; print $NF}' /proc/net/snmp 2>/dev/null | head -1; }  # RcvbufErrors
t0=$(date +%s)
while :; do
  el=$(( $(date +%s) - t0 ))
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "$el,,,,,,,,DEAD" >> "$OUT"; echo "[monitor] PROCESS $PID DIED at ${el}s"; break
  fi
  rss=$(awk '/VmRSS/{print int($2/1024)}' /proc/$PID/status 2>/dev/null)
  thr=$(ls /proc/$PID/task 2>/dev/null | wc -l)
  cpu=$(ps -p "$PID" -o %cpu= 2>/dev/null | tr -d ' ')
  load=$(awk '{print $1}' /proc/loadavg)
  drops=$(udp_drops)
  det=$([ -f "$ATK" ] && wc -l < "$ATK" 2>/dev/null || echo 0)
  err=$(grep -ciE 'error|fatal|terminate|cannot|deadlock|failed' "$LOG" 2>/dev/null || echo 0)
  echo "$el,$rss,$cpu,$thr,$load,${drops:-0},$det,$err,1" >> "$OUT"
  [ "$el" -ge "$SECS" ] && break
  sleep 5
done
echo "[monitor] wrote $OUT ($(wc -l < "$OUT") rows)"
# quick verdict
awk -F, 'NR>1 && $2!=""{if(r0=="")r0=$2; rN=$2; if($2>peak)peak=$2; if($9=="DEAD")dead=1}
  END{printf "[monitor] RSS first/peak/last = %s/%s/%s MB  growth=%s MB  %s\n",
      r0,peak,rN,rN-r0,(dead?"CRASHED":"survived")}' "$OUT"
