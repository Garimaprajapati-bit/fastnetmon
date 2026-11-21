#!/usr/bin/env bash
# Turnkey acceptance validation for the sharded parallel speed-calculation change (8-core VM).
# Read-only w.r.t. the repo (only builds + tests). Results + logs land in ./results_<ts>/.
#   FNM_SRC=/root/fastnetmon  bash validate_on_vm.sh
# Env overrides: DUR (load seconds, default 70), SOAK (soak seconds, default 900), DO_10M=1.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
KIT="$FNM_SRC/integration-kit"
BIN="$FNM_SRC/src/build_own/fastnetmon"
GEN="$KIT/netflow_gen.py"
LOG=/var/log/fastnetmon.log; CONF=/etc/fastnetmon.conf; NET=/etc/networks_list
OUT="$PWD/results_$(date +%Y%m%d_%H%M%S)"; mkdir -p "$OUT"
DUR="${DUR:-70}"; SOAK="${SOAK:-900}"; CLK=$(getconf CLK_TCK); NCPU=$(nproc)
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/run.log"; }
command -v curl >/dev/null || log "WARN: curl missing -> parsed_flows will be NA (apt-get install -y curl)"

prom(){ curl -s --max-time 3 http://127.0.0.1:9209/metrics 2>/dev/null; }
pmet(){ prom | awk -v m="$1" '$1==m{print $2; exit}'; }
udp(){ awk '/^Udp:/{if(h==""){for(i=2;i<=NF;i++)k[i]=$i;h=1}else{for(i=2;i<=NF;i++)v[k[i]]=$i}}END{printf "%s %s %s",v["InDatagrams"]+0,v["InErrors"]+0,v["RcvbufErrors"]+0}' /proc/net/snmp; }
cpuj(){ awk '{print $14+$15}' /proc/$1/stat 2>/dev/null; }     # utime+stime jiffies
rssmb(){ awk '/VmHWM/{print int($2/1024)}' /proc/$1/status 2>/dev/null; }

setup_conf(){ # $1 threads  $2 "subnet cidr(s) space separated"
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"; printf '%s\n' $2 > "$NET"; echo "1.0.0.0/8" > /etc/networks_whitelist
  sed -i -e 's/^netflow = .*/netflow = on/' -e 's/^netflow_host = .*/netflow_host = 127.0.0.1/' \
    -e 's/^enable_ban = .*/enable_ban = on/' -e 's/^enable_connection_tracking = .*/enable_connection_tracking = on/' \
    -e 's/^process_incoming_traffic = .*/process_incoming_traffic = on/' -e 's/^process_outgoing_traffic = .*/process_outgoing_traffic = on/' \
    -e 's/^prometheus = .*/prometheus = on/' "$CONF"
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$1" >> "$CONF"
  cat > /usr/local/bin/notify_about_attack.sh <<'N'
#!/bin/bash
echo "$(date -Is) ban $2" >> /var/log/fnm_attacks.log
N
  chmod +x /usr/local/bin/notify_about_attack.sh; : > /var/log/fnm_attacks.log
}
start_daemon(){ pkill -9 -x fastnetmon 2>/dev/null; sleep 3; : > "$LOG"
  "$BIN" --configuration_check >/dev/null 2>&1 || { log "CONFIG INVALID"; return 1; }
  nohup "$BIN" >>"$LOG" 2>&1 & echo $! > /tmp/fnm.pid
  for i in $(seq 1 180); do kill -0 "$(cat /tmp/fnm.pid)" 2>/dev/null || return 1
    ss -lun 2>/dev/null | grep -q ":2055" && return 0; sleep 1; done; return 1; }

# ---------- acceptance matrix row: hosts x threads, tracking+detection ON ----------
ACC="$OUT/acceptance_matrix.csv"
echo "label,monitored_hosts,active_hosts,threads,over_budget,peak_rss_mb,cpu_cores_used,cpu_util_pct,sent_flows,parsed_flows,recv_packets,udp_in_errors,udp_rcvbuf_errors,detections,survived" > "$ACC"
acc_run(){ # label  "networks"  active_hosts  gen_subnet  threads  attackers
  local lab=$1 nets=$2 ah=$3 gsub=$4 th=$5 att=${6:-0}
  local mon=0 c p; for c in $nets; do p="${c##*/}"; mon=$((mon+(1<<(32-p)))); done
  setup_conf "$th" "$nets"; start_daemon || { echo "$lab,$mon,$ah,$th,NA,NA,NA,NA,NA,NA,NA,NA,NA,NA,NO" >>"$ACC"; return; }
  local pid=$(cat /tmp/fnm.pid); sleep 2
  read UIN0 UER0 URC0 <<<"$(udp)"; local PF0=$(pmet fastnetmon_netflow_ipfix_all_protocols_total_flows) RP0=$(pmet fastnetmon_netflow_ipfix_total_packets)
  local j0=$(cpuj "$pid") t0=$(date +%s.%N)
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet "$gsub" --hosts "$ah" --rate 200000 --duration "$DUR" \
     --direction both --attackers "$att" --attacker-pps 600000 --seed 1 > "$OUT/gen_${lab}_t${th}.txt" 2>&1
  local t1=$(date +%s.%N) j1=$(cpuj "$pid")
  read UIN1 UER1 URC1 <<<"$(udp)"; local PF1=$(pmet fastnetmon_netflow_ipfix_all_protocols_total_flows) RP1=$(pmet fastnetmon_netflow_ipfix_total_packets)
  local wall=$(awk "BEGIN{print $t1-$t0}"); local cores=$(awk "BEGIN{printf \"%.2f\",($j1-$j0)/$CLK/$wall}"); local util=$(awk "BEGIN{printf \"%.1f\",($j1-$j0)/$CLK/$wall/$NCPU*100}")
  local ob=$(grep -c 'Traffic was calculated in' "$LOG"); local rss=$(rssmb "$pid"); local det=$(wc -l </var/log/fnm_attacks.log 2>/dev/null||echo 0)
  local sent=$(grep -oE 'done: [0-9]+' "$OUT/gen_${lab}_t${th}.txt"|grep -oE '[0-9]+'|head -1)
  local parsed=NA recv=NA; [ -n "${PF0:-}" ]&&[ -n "${PF1:-}" ]&&parsed=$(awk "BEGIN{print ${PF1%.*}-${PF0%.*}}")
  [ -n "${RP0:-}" ]&&[ -n "${RP1:-}" ]&&recv=$(awk "BEGIN{print ${RP1%.*}-${RP0%.*}}")
  local alive=$(pgrep -x fastnetmon >/dev/null&&echo yes||echo NO)
  echo "$lab,$mon,$ah,$th,$ob,$rss,$cores,$util,${sent:-NA},$parsed,$recv,$((UER1-UER0)),$((URC1-URC0)),$det,$alive" | tee -a "$ACC"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 2
}

log "=== 0. build daemon (documented) with the fix ==="
( cd "$FNM_SRC/src"; git apply "$HERE/get_top_k_reporting_fix.diff" 2>/dev/null; rm -rf build_own && mkdir build_own && cd build_own \
  && cmake .. -DLINK_WITH_ABSL=ON >/dev/null 2>&1 && make -j"$NCPU" fastnetmon >/dev/null 2>&1 ) && log "build OK" || { log "BUILD FAILED"; exit 1; }

log "=== 1. correctness: equivalence + concurrency (plain / TSan / ASan+UBSan) ==="
A="-I $FNM_SRC/src -I $FNM_SRC/src/tests $HERE/equiv_concurrency_test.cpp -lpthread -llog4cpp"
g++ -O2 -std=c++20 $A -o "$OUT/eqc" 2>>"$OUT/run.log" && "$OUT/eqc" | tee "$OUT/correctness.txt"
g++ -O2 -g -std=c++20 -fsanitize=thread $A -o "$OUT/eqc_t" 2>>"$OUT/run.log" && "$OUT/eqc_t" >"$OUT/tsan.txt" 2>&1 && log "container TSan clean" || log "container TSan: see tsan.txt"
g++ -O1 -g -std=c++20 -fsanitize=address,undefined $A -o "$OUT/eqc_a" 2>>"$OUT/run.log" && "$OUT/eqc_a" >"$OUT/asan.txt" 2>&1 && log "ASan/UBSan clean" || log "ASan/UBSan: see asan.txt"

log "=== 2. detection regression: parallel ON vs OFF identical (real daemon) ==="
FNM_SRC="$FNM_SRC" FNM_BIN="$BIN" bash "$HERE/regression_detection.sh" >"$OUT/regression.txt" 2>&1; tail -2 "$OUT/regression.txt" | tee -a "$OUT/run.log"

log "=== 3. ACCEPTANCE MATRIX: 1M x {1,2,4,8}, tracking+detection ON (20 attackers) ==="
for th in 1 2 4 8; do acc_run 1M "10.0.0.0/12" 1048576 10.0.0.0/12 "$th" 20; done

avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
if [ "${DO_10M:-1}" = 1 ] && [ "$avail" -gt 20000 ]; then
  log "=== 3b. ACCEPTANCE MATRIX: ~10M x {1,2,4,8} (MemAvailable ${avail}MB) ==="
  for th in 1 2 4 8; do acc_run 10M "10.0.0.0/9 10.128.0.0/11" 8388608 10.0.0.0/9 "$th" 20; done
else log "SKIP 10M matrix (MemAvailable ${avail}MB < 20000 or DO_10M=0)"; fi

log "=== 4. stress + soak (8 threads, tracking+detection ON) ==="
setup_conf 8 "10.0.0.0/12"; start_daemon && { pid=$(cat /tmp/fnm.pid)
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/12 --hosts 1048576 --rate 400000 --duration 180 --direction both --attackers 50 --attacker-pps 800000 --seed 2 >/dev/null 2>&1
  log "stress 1M survived=$(pgrep -x fastnetmon>/dev/null&&echo yes||echo NO) over_budget=$(grep -c 'Traffic was calculated in' "$LOG")"
  rss_a=$(rssmb "$pid")
  python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/12 --hosts 1048576 --rate 200000 --duration "$SOAK" --direction both --attackers 10 --attacker-pps 600000 --seed 3 >/dev/null 2>&1
  rss_b=$(rssmb "$pid")
  log "soak ${SOAK}s survived=$(pgrep -x fastnetmon>/dev/null&&echo yes||echo NO) rss ${rss_a}->${rss_b} MB (growth $((rss_b-rss_a)) MB)"
  pkill -9 -x fastnetmon; }

log "=== 5. daemon ThreadSanitizer: races introduced by the feature (ON vs OFF) ==="
( cd "$FNM_SRC/src"; rm -rf build_tsan && mkdir build_tsan && cd build_tsan
  cmake .. -DLINK_WITH_ABSL=ON -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" >/dev/null 2>&1 && make -j"$NCPU" fastnetmon >/dev/null 2>&1 ) \
&& { TB="$FNM_SRC/src/build_tsan/fastnetmon"
  for th in 4 1; do setup_conf "$th" "10.0.0.0/18"; pkill -9 -x fastnetmon; sleep 2; : >"$LOG"; rm -f /tmp/dt_$th.*
    TSAN_OPTIONS="halt_on_error=0 log_path=/tmp/dt_$th" nohup "$TB" >>"$LOG" 2>&1 &
    for i in $(seq 1 180); do pgrep -x fastnetmon>/dev/null||break; ss -lun 2>/dev/null|grep -q ":2055"&&break; sleep 1; done
    python3 "$GEN" --host 127.0.0.1 --port 2055 --subnet 10.0.0.0/18 --hosts 16384 --rate 40000 --duration 40 --direction both --attackers 20 --attacker-pps 600000 --seed 1 >/dev/null 2>&1
    sleep 3; pkill -9 -x fastnetmon; sleep 1
    cat /tmp/dt_$th.* 2>/dev/null | grep -oE "/[a-zA-Z_./]+\.(cpp|hpp):[0-9]+" | sort -u > "$OUT/tsan_sites_$th.txt"
    log "daemon TSan threads=$th: warnings=$(cat /tmp/dt_$th.* 2>/dev/null|grep -c 'WARNING: ThreadSanitizer')"; done
  log "race sites introduced by the feature (ON-only): $(comm -23 "$OUT/tsan_sites_4.txt" "$OUT/tsan_sites_1.txt" | grep -c .)  (expect 0)"
  comm -23 "$OUT/tsan_sites_4.txt" "$OUT/tsan_sites_1.txt" > "$OUT/tsan_feature_introduced_sites.txt"; } || log "daemon TSan step skipped (toolchain)"

log "=== DONE -> $OUT ==="; echo; column -s, -t < "$ACC"; ls -la "$OUT"
