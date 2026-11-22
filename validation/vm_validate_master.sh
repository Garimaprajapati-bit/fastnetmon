#!/usr/bin/env bash
# =============================================================================
# FastNetMon 10M parallel-recalc — VM validation driver (Stages 2,4-6,10-core)
# Run on the 8-core/32GB VM. Produces a timestamped results dir with raw logs +
# CSV + a summary. Measures the REAL integrated daemon (real allocator, real
# recalculate_speed, real detection), not a synthetic sweep.
#
# Usage:
#   FNM_SRC=/path/to/fastnetmon bash vm_validate_master.sh [stage]
#   stage = env | init | scale | correctness | all   (default: all)
#
# Requires: a daemon built WITH the 10M patch at $FNM_SRC/src/build_own/fastnetmon
#   (build: see BUILD section below). g++ for the correctness tests. Root (binds
#   :2055, writes /var/log, sets /etc/networks_list). bc/awk, standard coreutils.
# Optional (richer CPU stats): sysstat (mpstat/pidstat). Script works without them.
# =============================================================================
set -u
FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
FNM_BIN="${FNM_BIN:-$FNM_SRC/src/build_own/fastnetmon}"
STAGE="${1:-all}"
TESTS_DIR="${TESTS_DIR:-$(cd "$(dirname "$0")" && pwd)}"   # where the handover scripts live
TS=$(date +%Y%m%d_%H%M%S)
OUT="${OUT:-$PWD/vm_results_$TS}"; mkdir -p "$OUT"
DLOG=/var/log/fastnetmon.log
CONF=/tmp/fnm_vm.conf
SUMMARY="$OUT/SUMMARY.txt"
CSV="$OUT/scaling.csv"

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$SUMMARY"; }

# Host-count presets via real customer prefixes (daemon's own allocator populates them).
networks_for(){ case "$1" in
  1M)  echo "10.0.0.0/12";;                      # 1,048,576
  5M)  echo "10.0.0.0/10 10.64.0.0/12";;         # 4,194,304 + 1,048,576 = 5,242,880
  10M) echo "10.0.0.0/9 10.128.0.0/11";;         # 8,388,608 + 2,097,152 = 10,485,760
  *)   echo "$1";; esac; }

pctl(){ # $1=percentile(0-100) ; reads ALREADY-SORTED numbers on stdin
  awk -v p="$1" '{a[NR]=$1} END{ if(NR==0){print "NA";exit} i=int((p/100)*(NR-1))+1; print a[i]}'; }

# ---- Stage 2: environment report -------------------------------------------
env_report(){
  log "===== Stage 2: ENVIRONMENT ====="
  { echo "## CPU"; lscpu 2>/dev/null || cat /proc/cpuinfo | grep -E "model name|processor" | head;
    echo "## cores: nproc=$(nproc)";
    echo "## affinity: $(taskset -cp $$ 2>/dev/null)";
    echo "## cgroup cpu.max: $(cat /sys/fs/cgroup/cpu.max 2>/dev/null || cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null)";
    echo "## MEM"; free -h; echo "## DISK"; df -h / ;
    echo "## KERNEL"; uname -a; cat /etc/os-release 2>/dev/null | head -2;
    echo "## NUMA"; (numactl -H 2>/dev/null || echo "numactl not installed");
    echo "## freq governor"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "n/a";
    echo "## virt"; systemd-detect-virt 2>/dev/null || echo "n/a";
    echo "## tools"; for t in htop mpstat pidstat vmstat sar perf iperf3 stress-ng numactl g++ taskset; do printf "%-10s %s\n" "$t" "$(command -v $t || echo MISSING)"; done;
    echo "## existing fastnetmon"; pgrep -ax fastnetmon || echo "none running";
    echo "## daemon binary"; ls -la "$FNM_BIN" 2>/dev/null || echo "NOT BUILT: $FNM_BIN";
  } | tee "$OUT/environment.txt"
  # dedicated-vs-shared CPU sanity: 8-thread fixed-work burn; flat wall => real parallel cores
  cat > /tmp/burn.cpp <<'EOF'
#include <thread>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <chrono>
int main(int c,char**v){int T=c>1?atoi(v[1]):1;auto t0=std::chrono::steady_clock::now();
std::vector<std::thread>th;std::atomic<long>s{0};
for(int i=0;i<T;i++)th.emplace_back([&]{double x=1;for(long j=0;j<2000000000L;j++){x=x*1.0000001+1;if(x>1e9)x-=1e9;}s+=(long)x;});
for(auto&t:th)t.join();
printf("burn threads=%d wall_ms=%.0f (flat across T => dedicated cores)\n",T,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());}
EOF
  g++ -O2 -std=c++17 /tmp/burn.cpp -o /tmp/burn -lpthread 2>/dev/null && for T in 1 2 4 8; do /tmp/burn $T; done | tee -a "$OUT/environment.txt"
  log "env report -> $OUT/environment.txt"
}

# ---- config writer ----------------------------------------------------------
write_conf(){ # $1=networks  $2=ban(on/off)  $3=conntrack(on/off)  $4=threads
  cp "$FNM_SRC/src/fastnetmon.conf" "$CONF"
  printf '%s\n' $1 > /etc/networks_list; : > /etc/networks_whitelist
  sed -i -e 's#^netflow = .*#netflow = on#' -e 's#^netflow_host = .*#netflow_host = 127.0.0.1#' \
         -e "s#^enable_ban = .*#enable_ban = $2#" -e 's#^enable_ban_ipv6 = .*#enable_ban_ipv6 = off#' \
         -e "s#^enable_connection_tracking = .*#enable_connection_tracking = $3#" \
         -e 's#^process_incoming_traffic = .*#process_incoming_traffic = on#' \
         -e 's#^average_calculation_time = .*#average_calculation_time = 5#' "$CONF"
  sed -i '/speed_calculation_threads/d;/speed_calculation_shards/d' "$CONF"
  printf 'speed_calculation_threads = %s\nspeed_calculation_shards = 0\n' "$4" >> "$CONF"
}

# ---- one daemon run: returns samples + shard dist + rss + cpu --------------
# writes a line to $CSV ; raw log copied to $OUT
run_one(){ # $1=scale $2=ban $3=conntrack $4=threads $5=settle
  local scale="$1" ban="$2" ct="$3" th="$4" settle="${5:-25}"
  write_conf "$(networks_for $scale)" "$ban" "$ct" "$th"
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1; rm -f "$DLOG"
  local t_start=$(date +%s)
  FNM_BENCH_LOG=1 FNM_BENCH_SHARD_DIST=1 nohup "$FNM_BIN" --configuration_file "$CONF" >/tmp/fnm_vm_out.txt 2>&1 &
  local pid=$!
  # wait for allocation + first sweep
  local ok=0
  for i in $(seq 1 600); do
    kill -0 $pid 2>/dev/null || { log "  DAEMON DIED scale=$scale ban=$ban ct=$ct th=$th"; tail -4 /tmp/fnm_vm_out.txt | tee -a "$SUMMARY"; grep -iE "error|alloc" "$DLOG" 2>/dev/null | tail -3 | tee -a "$SUMMARY"; return 1; }
    grep -q "FNM_BENCH recalc_ms" "$DLOG" 2>/dev/null && { ok=1; break; }; sleep 1
  done
  [ "$ok" = 0 ] && { log "  no sweep scale=$scale th=$th"; pkill -9 -x fastnetmon; return 1; }
  local init_done=$(date +%s); local init_s=$((init_done - t_start))
  # peak RSS (VmHWM) + current
  local rss_hwm=$(awk '/VmHWM/{print int($2/1024)}' /proc/$pid/status 2>/dev/null)
  # CPU over the settle window (whole process)
  local u1 s1 u2 s2; read -r u1 s1 < <(awk '{print $14,$15}' /proc/$pid/stat 2>/dev/null)
  sleep "$settle"
  read -r u2 s2 < <(awk '{print $14,$15}' /proc/$pid/stat 2>/dev/null)
  local hz=$(getconf CLK_TCK); local cpus=$(awk -v a=$u1 -v b=$s1 -v c=$u2 -v d=$s2 -v hz=$hz -v w=$settle 'BEGIN{printf "%.2f",((c+d)-(a+b))/hz/w}')
  local rss_now=$(awk '/VmRSS/{print int($2/1024)}' /proc/$pid/status 2>/dev/null)
  # recalc_ms samples (drop first 2 warmup, keep the rest as measured)
  grep -oE "FNM_BENCH recalc_ms=[0-9.]+" "$DLOG" | awk -F= '{print $2}' | tail -n +3 > /tmp/samples.txt
  local nsamp=$(wc -l < /tmp/samples.txt)
  local mn=$(sort -n /tmp/samples.txt | head -1)
  local md=$(sort -n /tmp/samples.txt | awk '{a[NR]=$1}END{print a[int((NR+1)/2)]}')
  local p95=$(sort -n /tmp/samples.txt | pctl 95)
  local mx=$(sort -n /tmp/samples.txt | tail -1)
  local hosts=$(grep -oE "hosts=[0-9]+" "$DLOG" | tail -1 | cut -d= -f2)
  local dist=$(grep "FNM_BENCH_SHARD_DIST" "$DLOG" | tail -1 | sed 's/.*FNM_BENCH_SHARD_DIST //')
  local warns=$(grep -c "Could not get customer" "$DLOG" 2>/dev/null)
  echo "$scale,$ban,$ct,$th,$hosts,$init_s,$rss_hwm,$rss_now,$cpus,$nsamp,$mn,$md,$p95,$mx,$warns" >> "$CSV"
  log "  scale=$scale ban=$ban ct=$ct th=$th | hosts=$hosts init=${init_s}s rssHWM=${rss_hwm}MB cpus=$cpus | recalc_ms min=$mn med=$md p95=$p95 max=$mx (n=$nsamp)"
  [ -n "$dist" ] && echo "    shard_dist[$scale th=$th ct=$ct]: $dist" | tee -a "$SUMMARY"
  cp "$DLOG" "$OUT/log_${scale}_ban${ban}_ct${ct}_th${th}.log" 2>/dev/null
  pkill -9 -x fastnetmon 2>/dev/null; sleep 1
}

init_stage(){
  log "===== Stage 4-5: HOST INITIALIZATION (real allocator) ====="
  echo "scale,ban,conntrack,threads,hosts,init_s,rss_hwm_mb,rss_now_mb,cpus_busy,samples,recalc_min,recalc_med,recalc_p95,recalc_max,warns" > "$CSV"
  for scale in 1M 5M 10M; do
    log "-- init $scale (threads=1, conntrack off) --"
    run_one "$scale" on off 1 12
  done
  log "Init done. Verify: hosts == configured count, init time, rss_hwm, shard dist (min/max/cv/empty)."
}

scale_stage(){
  log "===== Stage 6: RECALC SCALABILITY 1/2/4/8 ====="
  [ -s "$CSV" ] || echo "scale,ban,conntrack,threads,hosts,init_s,rss_hwm_mb,rss_now_mb,cpus_busy,samples,recalc_min,recalc_med,recalc_p95,recalc_max,warns" > "$CSV"
  for scale in 1M 5M 10M; do
    for ct in off on; do
      for th in 1 2 4 8; do
        run_one "$scale" on "$ct" "$th" 22
      done
      log "-- speedup $scale conntrack=$ct (min-based): compute T(1)/T(N) from CSV --"
    done
  done
  # speedup/efficiency table from CSV (min column)
  log "--- SPEEDUP & EFFICIENCY (min recalc_ms) ---"
  awk -F, 'NR>1 && $2=="on"{key=$1"_"$3; t[key"_"$4]=$11}
    END{ split("1M 5M 10M",S," "); split("off on",C," ");
      for(si in S){for(ci in C){k=S[si]"_"C[ci]; b=t[k"_1"];
        if(b!=""){ printf "scale=%s conntrack=%s  ", S[si], C[ci];
          for(n=1;n<=8;n*=2){v=t[k"_"n]; if(v!=""){sp=b/v; printf "t%d=%s(%.2fx,%.0f%%) ", n, v, sp, sp/n*100}} print ""}}}}' "$CSV" | tee -a "$SUMMARY"
}

correctness_stage(){
  log "===== Stage 10 (core): CORRECTNESS ====="
  # equivalence + concurrency (ASan/UBSan + TSan if time)
  if [ -f "$TESTS_DIR/equiv_concurrency_test.cpp" ]; then
    g++ -O2 -std=c++20 -I "$FNM_SRC/src" -I "$FNM_SRC/src/tests" "$TESTS_DIR/equiv_concurrency_test.cpp" -o /tmp/eqc -lpthread -llog4cpp 2>/dev/null \
      && { log "equivalence/concurrency:"; /tmp/eqc | tee -a "$SUMMARY"; }
    g++ -O1 -g -std=c++20 -fsanitize=address,undefined -I "$FNM_SRC/src" -I "$FNM_SRC/src/tests" "$TESTS_DIR/equiv_concurrency_test.cpp" -o /tmp/eqc_asan -lpthread -llog4cpp 2>/dev/null \
      && { ASAN_OPTIONS=detect_leaks=1 /tmp/eqc_asan >/tmp/asan.txt 2>&1; log "ASan/UBSan exit=$? errors=$(grep -cE 'ERROR|runtime error' /tmp/asan.txt)"; }
    g++ -O1 -g -std=c++20 -fsanitize=thread -I "$FNM_SRC/src" -I "$FNM_SRC/src/tests" "$TESTS_DIR/equiv_concurrency_test.cpp" -o /tmp/eqc_tsan -lpthread -llog4cpp 2>/dev/null \
      && { TSAN_OPTIONS=halt_on_error=0 /tmp/eqc_tsan >/tmp/tsan.txt 2>&1; log "counter-class TSan exit=$? warnings=$(grep -c 'WARNING: ThreadSanitizer' /tmp/tsan.txt)"; }
  else log "equiv_concurrency_test.cpp not found in $TESTS_DIR"; fi
  # multi-shard perf unit test (redistribution correctness)
  local pt=$(find "$FNM_SRC/src/build_own" -name speed_counters_performance_test -type f 2>/dev/null | head -1)
  [ -n "$pt" ] && { log "perf unit test:"; timeout 180 "$pt" 2>&1 | grep -iE "misplaced|checks passed|Calculated speed" | tee -a "$SUMMARY"; }
  # detection parity (real daemon, parallel ON vs OFF)
  if [ -f "$TESTS_DIR/regression_detection.sh" ]; then
    log "detection parity (threads=1 vs 4):"
    FNM_SRC="$FNM_SRC" FNM_BIN="$FNM_BIN" bash "$TESTS_DIR/regression_detection.sh" 2>&1 | grep -E "detected_unique|REGRESSION|IDENTICAL|DIFFERENCE" | tee -a "$SUMMARY"
  fi
}

log "FNM_SRC=$FNM_SRC  FNM_BIN=$FNM_BIN  OUT=$OUT  STAGE=$STAGE"
[ -x "$FNM_BIN" ] || { log "ERROR: daemon not found/executable at $FNM_BIN — build it first (see header)"; }
case "$STAGE" in
  env) env_report;;
  init) env_report; init_stage;;
  scale) scale_stage;;
  correctness) correctness_stage;;
  all) env_report; init_stage; scale_stage; correctness_stage; log "ALL STAGES DONE -> $OUT";;
  *) log "unknown stage: $STAGE (use env|init|scale|correctness|all)";;
esac
