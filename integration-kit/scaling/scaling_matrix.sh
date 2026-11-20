#!/usr/bin/env bash
# scaling_matrix.sh (v2) — measure recalculate_speed() scaling in the REAL daemon across
# monitored-host counts x worker threads, WITH flow tracking + reporting enabled, with
# repetitions so variance is visible. Isolated VM only.
#
# Question it answers: does adding sweep workers (1->2->4->8) actually reduce recalc wall
# time in the integrated daemon at 1M and at ~10M hosts, WITHOUT more packet loss or any
# change in detection? The 1s budget is a target to MEASURE against, not an assumption.
#
# It applies two things to the working tree:
#   1. the production top-k fix (get_top_k_reporting_fix.diff) if not already applied;
#   2. a one-line unconditional timing log in recalculate_speed() — EXPERIMENTAL, NOT FOR
#      COMMIT (the upstream watchdog only logs when a recalc EXCEEDS 1s, so under-budget
#      times are otherwise invisible). Revert with: git checkout -- src/fastnetmon_logic.cpp
#
# Prereqs:  export FNM_BIN=/root/fastnetmon/src/build/fastnetmon FNM_SRC=/root/fastnetmon
# Run:      bash scaling_matrix.sh
#
# Tunables (env):
#   THREAD_LIST="1 2 4 8"   REPS=3   DUR=60   (seconds per run)
#   HOST_ROWS  = pipe-delimited rows: label|monitor_cidrs|gen_subnet|gen_hosts|rate
#     monitor_cidrs may be several space-separated CIDRs (summed = monitored host count).
set -u

FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
FNM_BIN="${FNM_BIN:-$FNM_SRC/src/build/fastnetmon}"
export FNM_BIN FNM_SRC
KIT_DIR="${KIT_DIR_OVERRIDE:-$FNM_SRC/integration-kit}"
FIX_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG=/var/log/fastnetmon.log
NETWORKS=/etc/networks_list
OUT="${OUT:-$PWD/scaling_results.csv}"
THREAD_LIST="${THREAD_LIST:-1 2 4 8}"
REPS="${REPS:-3}"
DUR="${DUR:-60}"
RAM_PER_M_MB="${RAM_PER_M_MB:-1600}"   # ~1.5-1.6 GB of RSS per 1M pre-allocated hosts

# label | monitored CIDR(s) | generator subnet | generator active hosts | offered rate (flows/s)
HOST_ROWS="${HOST_ROWS:-\
1M|10.0.0.0/12|10.0.0.0/12|1000000|200000
10M|10.0.0.0/9 10.128.0.0/11|10.0.0.0/9|8000000|200000}"

file="$FNM_SRC/src/fastnetmon_logic.cpp"
marker="SCALING-INSTRUMENTATION recalc timing"

hosts_in_cidrs() {  # sum 2^(32-prefix) for space-separated CIDRs
  local total=0 c p
  for c in $1; do p="${c##*/}"; total=$(( total + (1 << (32 - p)) )); done
  echo "$total"
}
read_udp() {  # InDatagrams InErrors RcvbufErrors
  awk '/^Udp:/{ if(h==""){for(i=2;i<=NF;i++)k[i]=$i; h=1} else {for(i=2;i<=NF;i++)v[k[i]]=$i} }
       END{ printf "%s %s %s", v["InDatagrams"]+0, v["InErrors"]+0, v["RcvbufErrors"]+0 }' /proc/net/snmp
}
mem_avail_mb() { awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo; }

# ---- 1. production fix ----
echo ">> ensuring production top-k fix is applied"
if ! grep -q "partial_sort_copy(s->average_speed_map" "$FNM_SRC/src/abstract_subnet_counters.hpp"; then
  ( cd "$FNM_SRC" && git apply "$FIX_DIR/get_top_k_reporting_fix.diff" ) && echo "   applied" \
    || { echo "   !! could not apply top-k fix — apply get_top_k_reporting_fix.diff manually"; exit 1; }
else echo "   already present"; fi

# ---- 2. experimental timing log (idempotent, not for commit) ----
echo ">> inserting experimental recalc-timing log (revert before committing)"
if ! grep -q "$marker" "$file"; then
  python3 - "$file" "$marker" <<'PY'
import sys
path, marker = sys.argv[1], sys.argv[2]
src = open(path).read()
anchor = "    // Report cases when we calculate speed too slow"
ins = ('    // [%s] unconditional per-sweep wall time; revert with git checkout before committing\n'
       '    logger << log4cpp::Priority::ERROR << "RECALC ms=" '
       '<< (long)(speed_calculation_time.tv_sec * 1000 + speed_calculation_time.tv_usec / 1000);\n\n') % marker
assert anchor in src, "anchor not found — insert timing log manually"
open(path, "w").write(src.replace(anchor, ins + anchor, 1))
print("   inserted")
PY
else echo "   already present"; fi

# ---- 3. build ----
echo ">> rebuilding daemon (documented build)"
( cd "$FNM_SRC/src/build" && make -j"$(nproc)" ) || { echo "build failed"; exit 1; }

# ---- runtime estimate ----
nrows=$(printf '%s\n' "$HOST_ROWS" | grep -c .); nthr=$(echo $THREAD_LIST | wc -w)
echo ">> plan: ${nrows} host-rows x ${nthr} thread-settings x ${REPS} reps = $(( nrows * nthr * REPS )) runs"
echo ">> rough runtime: each run ~ (prealloc + ${DUR}s load + ~15s); 10M prealloc is the slow part"

echo "label,monitored_hosts,active_hosts,threads,rep,recalc_min_ms,recalc_med_ms,recalc_p95_ms,recalc_max_ms,over_budget,samples,peak_rss_mb,avg_cpu_pct,peak_cpu_pct,udp_in_datagrams,udp_in_errors,udp_rcvbuf_errors,sent_flows,detections" > "$OUT"
echo ">> per-run results -> $OUT"
cd "$KIT_DIR" || { echo "kit dir $KIT_DIR not found"; exit 1; }

printf '%s\n' "$HOST_ROWS" | while IFS='|' read -r LABEL MON_CIDRS GEN_SUBNET GEN_HOSTS RATE; do
  [ -z "${LABEL:-}" ] && continue
  MONITORED=$(hosts_in_cidrs "$MON_CIDRS")
  need_mb=$(( MONITORED / 1000000 * RAM_PER_M_MB + 2048 ))
  avail_mb=$(mem_avail_mb)
  echo "===================================================================="
  echo ">> row $LABEL: monitored=$MONITORED ($MON_CIDRS) active=$GEN_HOSTS rate=$RATE  need~${need_mb}MB avail=${avail_mb}MB"
  if [ "$avail_mb" -lt "$need_mb" ]; then
    echo "   !! SKIP $LABEL — not enough RAM (need ~${need_mb}MB, have ${avail_mb}MB). Use a bigger VM or smaller monitored set."
    continue
  fi
  export SUBNET="$GEN_SUBNET"
  ./run_integration.sh setup >/dev/null
  printf '%s\n' $MON_CIDRS > "$NETWORKS"    # override monitored set (may be multiple CIDRs)

  for TH in $THREAD_LIST; do
    for REP in $(seq 1 "$REPS"); do
      echo ">>> $LABEL  threads=$TH  rep=$REP/$REPS"
      pkill -9 -x fastnetmon 2>/dev/null; sleep 3
      pgrep -x fastnetmon >/dev/null && { echo "   ZOMBIES — abort"; exit 1; }
      ./run_integration.sh start "$TH" >/dev/null || { echo "   start failed, skipping"; continue; }
      read UIN0 UERR0 URCV0 <<<"$(read_udp)"
      ./run_integration.sh load "$GEN_HOSTS" "$RATE" "$DUR" > /tmp/load_out.txt 2>&1
      read UIN1 UERR1 URCV1 <<<"$(read_udp)"
      sent=$(grep -oE 'done: [0-9]+' /tmp/load_out.txt | grep -oE '[0-9]+' | head -1)

      stats=$(grep -oE 'RECALC ms=[0-9]+' "$LOG" | grep -oE '[0-9]+' | sort -n | \
        awk 'BEGIN{n=0}{a[n++]=$1; if($1>=1000)ob++}
             END{ if(n==0){print "0,0,0,0,0,0"; exit}
                  med=a[int(0.50*n)]; p95=a[int(0.95*n)]; if(p95=="")p95=a[n-1];
                  printf "%d,%d,%d,%d,%d,%d", a[0], med, p95, a[n-1], ob+0, n }')
      csv="$KIT_DIR/results_load.csv"
      peak_rss=$(awk -F, 'NR>1 && $2!=""{if($2>m)m=$2}END{print m+0}' "$csv" 2>/dev/null)
      avg_cpu=$(awk -F, 'NR>1 && $3!=""{s+=$3;n++}END{print (n?int(s/n):0)}' "$csv" 2>/dev/null)
      peak_cpu=$(awk -F, 'NR>1 && $3!=""{if($3>m)m=$3}END{print m+0}' "$csv" 2>/dev/null)
      det=$(awk -F, 'NR>1{d=$7}END{print d+0}' "$csv" 2>/dev/null)
      dIN=$((UIN1-UIN0)); dERR=$((UERR1-UERR0)); dRCV=$((URCV1-URCV0))

      echo "$LABEL,$MONITORED,$GEN_HOSTS,$TH,$REP,$stats,$peak_rss,$avg_cpu,$peak_cpu,$dIN,$dERR,$dRCV,${sent:-NA},$det" | tee -a "$OUT"
    done
  done
  pkill -9 -x fastnetmon 2>/dev/null; sleep 2
done

echo "===================================================================="
echo ">> DONE. Per-run matrix:"; column -s, -t < "$OUT"
echo
echo ">> Median recalc (ms) across reps, by host-count x threads:"
awk -F, 'NR>1{key=$1" t"$4; v[key]=v[key]" "$7}
  END{ for(k in v){ n=split(v[k],x," "); for(i=1;i<=n;i++)for(j=i+1;j<=n;j++)if(x[j]<x[i]){t=x[i];x[i]=x[j];x[j]=t};
       printf "  %-10s median-of-rep-medians = %s ms\n", k, x[int((n+1)/2)] } }' "$OUT" | sort
echo
echo ">> Interpreting: within a host-count, recalc_med should FALL as threads rise if the sweep"
echo "   parallelises; udp_in_errors/udp_rcvbuf_errors must stay 0 (no silent input loss);"
echo "   detections must match across thread counts (behaviour unchanged). received flows ~ sent"
echo "   when the two udp error deltas are 0."
echo ">> REMEMBER: revert the timing log before committing:"
echo "   (cd $FNM_SRC && git checkout -- src/fastnetmon_logic.cpp)"
