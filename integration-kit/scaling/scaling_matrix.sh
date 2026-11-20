#!/usr/bin/env bash
# scaling_matrix.sh (v3) — measure recalculate_speed() scaling in the REAL daemon across
# monitored-host counts x worker threads, WITH flow tracking + reporting enabled, with
# repetitions. Isolated VM only.
#
# Question: does adding sweep workers (1->2->4->8) reduce recalc wall time in the integrated
# daemon at 1M and ~10M hosts, WITHOUT losing input or changing detection? The 1s budget is a
# target to MEASURE against, not an assumption — 10M may legitimately exceed it; report honestly.
#
# Applies to the working tree: (1) the production top-k fix (get_top_k_reporting_fix.diff) if not
# present; (2) a one-line unconditional recalc-timing log — EXPERIMENTAL, NOT FOR COMMIT (the
# upstream watchdog only logs when a recalc EXCEEDS 1s). Revert: git checkout -- src/fastnetmon_logic.cpp
#
# Input accounting (answers "did every sent flow reach + get parsed by the daemon"):
#   sent_flows       : generator "done: N"
#   recv_packets     : daemon Prometheus fastnetmon_netflow_ipfix_total_packets delta
#   parsed_flows     : daemon Prometheus fastnetmon_netflow_ipfix_all_protocols_total_flows delta
#   flow_loss        : sent_flows - parsed_flows   (0 => every generated flow was parsed)
#   udp_in_errors / udp_rcvbuf_errors : /proc/net/snmp deltas (kernel drops; 0 != proof of 0 loss,
#                      which is why we also carry parsed_flows as the authoritative processed count)
# Memory safety: per-row RAM preflight + refuses to start under existing swap pressure; records
#   per-run swap in/out pages and OOM-kill events.
#
# Prereqs:  export FNM_BIN=/root/fastnetmon/src/build/fastnetmon FNM_SRC=/root/fastnetmon
#           apt-get install -y curl   (for the Prometheus cross-check)
# Run 1M first:  HOST_ROWS="1M|10.0.0.0/12|10.0.0.0/12|1048576|200000" bash scaling_matrix.sh
# Then (after inspecting 1M) the default runs 1M + ~10M.
set -u

FNM_SRC="${FNM_SRC:-/root/fastnetmon}"
FNM_BIN="${FNM_BIN:-$FNM_SRC/src/build/fastnetmon}"; export FNM_BIN FNM_SRC
KIT_DIR="${KIT_DIR_OVERRIDE:-$FNM_SRC/integration-kit}"
FIX_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF=/etc/fastnetmon.conf; NETWORKS=/etc/networks_list; LOG=/var/log/fastnetmon.log
OUT="${OUT:-$PWD/scaling_results.csv}"
THREAD_LIST="${THREAD_LIST:-1 2 4 8}"; REPS="${REPS:-3}"; DUR="${DUR:-60}"
RAM_PER_M_MB="${RAM_PER_M_MB:-1600}"; SWAP_PRESSURE_MB="${SWAP_PRESSURE_MB:-512}"; FORCE="${FORCE:-0}"
PROM_URL="${PROM_URL:-http://127.0.0.1:9209/metrics}"

# label | monitored CIDR(s) | generator subnet | generator ACTIVE hosts | offered rate (flows/s)
# NOTE: monitored (CIDR size) drives recalc sweep cost; active (generator) drives flow-tracking load.
# For 1M they match (1048576). For ~10M the generator spans one /9 (8388608 active) while 10,485,760
# are monitored — this is labelled, not hidden.
HOST_ROWS="${HOST_ROWS:-\
1M|10.0.0.0/12|10.0.0.0/12|1048576|200000
10M|10.0.0.0/9 10.128.0.0/11|10.0.0.0/9|8388608|200000}"

file="$FNM_SRC/src/fastnetmon_logic.cpp"; marker="SCALING-INSTRUMENTATION recalc timing"

hosts_in_cidrs(){ local t=0 c p; for c in $1; do p="${c##*/}"; t=$((t+(1<<(32-p)))); done; echo "$t"; }
read_udp(){ awk '/^Udp:/{if(h==""){for(i=2;i<=NF;i++)k[i]=$i;h=1}else{for(i=2;i<=NF;i++)v[k[i]]=$i}}
                 END{printf "%s %s %s", v["InDatagrams"]+0, v["InErrors"]+0, v["RcvbufErrors"]+0}' /proc/net/snmp; }
read_swap(){ awk '/^pswpin/{i=$2}/^pswpout/{o=$2}END{printf "%s %s", i+0, o+0}' /proc/vmstat; }
oom_count(){ local n; n=$(dmesg 2>/dev/null | grep -ciE 'out of memory|oom-kill|killed process'); echo "${n:-0}"; }
prom_metric(){ curl -s --max-time 3 "$PROM_URL" 2>/dev/null | awk -v m="$1" '$1==m{print $2; exit}'; }
mem_line(){ awk '/^MemAvailable/{ma=$2}/^SwapTotal/{st=$2}/^SwapFree/{sf=$2}
                 END{printf "%d %d", int(ma/1024), int((st-sf)/1024)}' /proc/meminfo; }

command -v curl >/dev/null || echo "!! curl not found — install it (apt-get install -y curl) for the parsed-flow cross-check; parsed_flows will be NA"

echo ">> ensuring production top-k fix is applied"
if ! grep -q "partial_sort_copy(s->average_speed_map" "$FNM_SRC/src/abstract_subnet_counters.hpp"; then
  ( cd "$FNM_SRC" && git apply "$FIX_DIR/get_top_k_reporting_fix.diff" ) && echo "   applied" \
    || { echo "   !! apply get_top_k_reporting_fix.diff manually"; exit 1; }
else echo "   already present"; fi

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

echo ">> rebuilding daemon (documented build)"
( cd "$FNM_SRC/src/build" && make -j"$(nproc)" ) || { echo "build failed"; exit 1; }

nrows=$(printf '%s\n' "$HOST_ROWS" | grep -c .); nthr=$(echo $THREAD_LIST|wc -w)
echo ">> plan: ${nrows} rows x ${nthr} threads x ${REPS} reps = $((nrows*nthr*REPS)) runs; ${DUR}s load each"

echo "label,monitored_hosts,active_hosts,threads,rep,recalc_min_ms,recalc_med_ms,recalc_p95_ms,recalc_max_ms,over_budget,samples,peak_rss_mb,avg_cpu_pct,peak_cpu_pct,sent_flows,parsed_flows,recv_packets,flow_loss,udp_in_errors,udp_rcvbuf_errors,swap_in_pg,swap_out_pg,oom_events,detections" > "$OUT"
echo ">> per-run results -> $OUT"
cd "$KIT_DIR" || { echo "kit dir $KIT_DIR missing"; exit 1; }

printf '%s\n' "$HOST_ROWS" | while IFS='|' read -r LABEL MON_CIDRS GEN_SUBNET GEN_HOSTS RATE; do
  [ -z "${LABEL:-}" ] && continue
  MONITORED=$(hosts_in_cidrs "$MON_CIDRS")
  need_mb=$(( MONITORED/1000000*RAM_PER_M_MB + 2048 ))
  read -r avail_mb swap_used_mb <<<"$(mem_line)"
  echo "===================================================================="
  echo ">> row $LABEL: monitored=$MONITORED ($MON_CIDRS) active=$GEN_HOSTS rate=$RATE | need~${need_mb}MB avail=${avail_mb}MB swap_used=${swap_used_mb}MB"
  if [ "$avail_mb" -lt "$need_mb" ]; then echo "   !! SKIP $LABEL — insufficient RAM (need ~${need_mb}MB, have ${avail_mb}MB)"; continue; fi
  if [ "$swap_used_mb" -gt "$SWAP_PRESSURE_MB" ] && [ "$FORCE" != "1" ]; then
    echo "   !! SKIP $LABEL — machine already under memory pressure (swap_used=${swap_used_mb}MB > ${SWAP_PRESSURE_MB}MB). Set FORCE=1 to override."; continue; fi

  export SUBNET="$GEN_SUBNET"
  ./run_integration.sh setup >/dev/null
  printf '%s\n' $MON_CIDRS > "$NETWORKS"          # monitored set (may be several CIDRs)
  sed -i -e 's/^prometheus = .*/prometheus = on/' -e 's/^prometheus_port = .*/prometheus_port = 9209/' "$CONF"
  grep -q '^prometheus = ' "$CONF" || echo 'prometheus = on' >> "$CONF"

  for TH in $THREAD_LIST; do
    for REP in $(seq 1 "$REPS"); do
      echo ">>> $LABEL threads=$TH rep=$REP/$REPS"
      pkill -9 -x fastnetmon 2>/dev/null; sleep 3
      pgrep -x fastnetmon >/dev/null && { echo "   ZOMBIES — abort"; exit 1; }
      ./run_integration.sh start "$TH" >/dev/null || { echo "   start failed"; continue; }
      sleep 2                                        # let prometheus thread bind + counters settle
      read UIN0 UERR0 URCV0 <<<"$(read_udp)"; read SWI0 SWO0 <<<"$(read_swap)"; OOM0=$(oom_count)
      PF0=$(prom_metric fastnetmon_netflow_ipfix_all_protocols_total_flows); RP0=$(prom_metric fastnetmon_netflow_ipfix_total_packets)
      ./run_integration.sh load "$GEN_HOSTS" "$RATE" "$DUR" > /tmp/load_out.txt 2>&1
      read UIN1 UERR1 URCV1 <<<"$(read_udp)"; read SWI1 SWO1 <<<"$(read_swap)"; OOM1=$(oom_count)
      PF1=$(prom_metric fastnetmon_netflow_ipfix_all_protocols_total_flows); RP1=$(prom_metric fastnetmon_netflow_ipfix_total_packets)
      sent=$(grep -oE 'done: [0-9]+' /tmp/load_out.txt | grep -oE '[0-9]+' | head -1)

      stats=$(grep -oE 'RECALC ms=[0-9]+' "$LOG" | grep -oE '[0-9]+' | sort -n | \
        awk 'BEGIN{n=0}{a[n++]=$1; if($1>=1000)ob++}
             END{if(n==0){print "0,0,0,0,0,0";exit} p95=a[int(0.95*n)]; if(p95=="")p95=a[n-1];
                 printf "%d,%d,%d,%d,%d,%d", a[0], a[int(0.50*n)], p95, a[n-1], ob+0, n}')
      csv="$KIT_DIR/results_load.csv"
      peak_rss=$(awk -F, 'NR>1&&$2!=""{if($2>m)m=$2}END{print m+0}' "$csv" 2>/dev/null)
      avg_cpu=$(awk -F, 'NR>1&&$3!=""{s+=$3;n++}END{print (n?int(s/n):0)}' "$csv" 2>/dev/null)
      peak_cpu=$(awk -F, 'NR>1&&$3!=""{if($3>m)m=$3}END{print m+0}' "$csv" 2>/dev/null)
      det=$(awk -F, 'NR>1{d=$7}END{print d+0}' "$csv" 2>/dev/null)
      parsed=NA; recv=NA; loss=NA
      [ -n "${PF0:-}" ] && [ -n "${PF1:-}" ] && parsed=$(awk "BEGIN{print ${PF1%.*}-${PF0%.*}}")
      [ -n "${RP0:-}" ] && [ -n "${RP1:-}" ] && recv=$(awk "BEGIN{print ${RP1%.*}-${RP0%.*}}")
      [ "$parsed" != NA ] && [ -n "${sent:-}" ] && loss=$(( sent - parsed ))
      dERR=$((UERR1-UERR0)); dRCV=$((URCV1-URCV0)); dSWI=$((SWI1-SWI0)); dSWO=$((SWO1-SWO0)); dOOM=$((OOM1-OOM0))

      echo "$LABEL,$MONITORED,$GEN_HOSTS,$TH,$REP,$stats,$peak_rss,$avg_cpu,$peak_cpu,${sent:-NA},$parsed,$recv,$loss,$dERR,$dRCV,$dSWI,$dSWO,$dOOM,$det" | tee -a "$OUT"
    done
  done
  pkill -9 -x fastnetmon 2>/dev/null; sleep 2
done

echo "===================================================================="
echo ">> DONE. Per-run matrix:"; column -s, -t < "$OUT"
echo
echo ">> Median recalc (ms) by host-count x threads (median of per-run medians across reps):"
awk -F, 'NR>1{k=$1" t"$4; v[k]=v[k]" "$7} END{for(k in v){n=split(v[k],x," ");
  for(i=1;i<=n;i++)for(j=i+1;j<=n;j++)if(x[j]<x[i]){t=x[i];x[i]=x[j];x[j]=t}
  printf "  %-10s = %s ms\n", k, x[int((n+1)/2)]}}' "$OUT" | sort
echo
echo ">> READ IT HONESTLY:"
echo "   - scaling: within a host-count, recalc_med should fall 1->2->4->8 IF the sweep parallelises."
echo "   - input integrity: flow_loss should be ~0 (sent==parsed). udp_* and swap_* give context;"
echo "     udp errors = 0 is NOT proof of zero loss — parsed_flows vs sent_flows is the real check."
echo "   - correctness here is NARROW: attackers=0, so detections must be 0 everywhere (no false"
echo "     positives). Proving detection EQUIVALENCE under attack needs a separate run with a fixed"
echo "     attacker set, comparing the banned-IP sets / notify events across thread counts (TODO)."
echo "   - workload is deterministic (generator seed fixed at 1), so runs are genuinely comparable."
echo ">> revert timing log before any commit:  (cd $FNM_SRC && git checkout -- src/fastnetmon_logic.cpp)"
