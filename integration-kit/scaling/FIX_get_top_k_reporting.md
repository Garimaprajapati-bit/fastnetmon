# Fix: O(hosts) allocation in sharded `get_top_k_average_speed` (top-talkers reporting)

**Status:** regression fixed and verified. Scaling across threads/host-counts NOT yet demonstrated
(see "What this does NOT prove" below). Fix is saved but **not committed/pushed** — push later.

- Branch: `sharded-speed-counters`
- File: `src/abstract_subnet_counters.hpp`, method `abstract_subnet_counters_t::get_top_k_average_speed`
- Patch: `get_top_k_reporting_fix.diff` (this folder) — `git apply` from repo root
- Correctness proof: `topk_equiv.cpp` (this folder)
- Full investigation trail: `../evidence/EVIDENCE.md`

---

## 1. The issue (symptom)

Running the sharded-speed-counters daemon with **connection tracking + reporting enabled** at
**1M monitored hosts** (networks_list `10.0.0.0/12`), NetFlow v5 at ~200k flows/s, 8 speed-calculation
threads on an 8-core VM, the once-per-second `recalculate_speed()` occasionally exceeded its
**1-second budget**. The upstream watchdog fired:

```
[ERROR] ALERT. Toolkit working incorrectly. We should calculate speed counters in <1 second
[ERROR] Traffic was calculated in: 1 sec 75521 microseconds
```

## 2. How it was found (evidence, not guesswork)

Earlier hypotheses (the flow-counter global lock; the EWMA sweep being CPU-bound) were **tested and
refuted**:
- gdb `thread apply all bt` under load: `increment_flow_counters` appeared **0 times** — the flow
  read/lock was not a hotspot. Removing the flow lock (an experimental snapshot-swap) did **not** help.
- `perf record -F 99 -g --call-graph dwarf` under load: **62% of daemon CPU was in
  `screen_draw_ipv4_thread -> draw_table_ipv4_hash`** (the console/log top-talkers table), and **~37%
  of that was page faults** (`do_anonymous_page -> alloc_anon_folio`) — i.e. first-touch of freshly
  allocated memory, every second.

## 3. Root cause

`draw_table_ipv4_hash()` runs every `check_period` (1 s) and calls `get_top_k_average_speed()` to pick
the top ~`max_ips_in_list` (~50) talkers. The **sharded** implementation did:

```cpp
std::vector<std::pair<T, Counter>> merged;
for (auto& s : shards) {
    std::lock_guard<std::mutex> lock_guard(s->mutex);
    std::copy(s->average_speed_map.begin(), s->average_speed_map.end(), std::back_inserter(merged));
}
std::partial_sort_copy(merged.begin(), merged.end(), vector_for_sort.begin(), vector_for_sort.end(), ...);
```

So to produce ~50 rows it **materialised a copy of every host entry across all shards (~1M, ~100+ MB)
every second**, via a growing `back_inserter` vector (repeated reallocation → the page-fault storm),
**while holding every shard mutex** during the copy — the same locks the parallel speed sweep needs
(reporting ⇆ sweep contention, which is why the parallel sweep didn't speed up with tracking on).

**This is a regression introduced by the sharding, not an upstream bug.** Upstream `751a596` sorts the
single live map straight into the ~50-slot output with **zero O(hosts) allocation**:

```cpp
std::lock_guard<std::mutex> lock_guard(this->counter_map_mutex);
std::partial_sort_copy(average_speed_map.begin(), average_speed_map.end(),
                       vector_for_sort.begin(), vector_for_sort.end(), comparator);
```

The `merged` copy only existed because sharding split the data across N maps and the simplest way to get
a global top-k was to concatenate them first.

**Scope:** `get_top_k_average_speed` feeds only the console/log top-talkers table (display). Ban/attack
detection uses a different callback (`speed_calculation_callback_local_ipv4/ipv6`), so detection
semantics are unaffected by this change.

## 4. The fix

Compute each shard's top-k **under its own lock** into a small reused buffer, then merge the per-shard
results and take the final top-k:

```cpp
void get_top_k_average_speed(std::vector<std::pair<T, Counter>>& vector_for_sort,
                             const attack_detection_threshold_type_t& sorter_type,
                             const attack_detection_direction_type_t& sort_direction) {
    const TrafficComparatorClass<std::pair<T, Counter>> comparator(sort_direction, sorter_type);
    const size_t k = vector_for_sort.size();

    std::vector<std::pair<T, Counter>> shard_top(k);          // reused, bounded by k (not by hosts)
    std::vector<std::pair<T, Counter>> merged;
    merged.reserve(shards.size() * k);                        // at most shards*k entries (tiny)

    for (auto& s : shards) {
        typename std::vector<std::pair<T, Counter>>::iterator shard_top_end;
        {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            shard_top_end = std::partial_sort_copy(s->average_speed_map.begin(), s->average_speed_map.end(),
                                                   shard_top.begin(), shard_top.end(), comparator);
        }
        merged.insert(merged.end(), shard_top.begin(), shard_top_end); // only what was written
    }

    std::partial_sort_copy(merged.begin(), merged.end(), vector_for_sort.begin(), vector_for_sort.end(),
                           comparator);
}
```

Why it's correct: **the global top-k of a union equals the top-k of the concatenation of each subset's
own top-k.** Any element in the global top-k has fewer than k elements strictly above it in the whole
set, hence fewer than k above it within its own shard, so it survives that shard's top-k and reaches
`merged`. Nothing is lost. (Tie order among equal-metric hosts may differ, but this is a display table
and upstream gives no tie-order guarantee either.)

Effects:
- allocation per call: **O(hosts) → O(shards·k)** (≈ hundreds of entries instead of ~1M);
- each shard lock held only for its own `partial_sort_copy`, with **no allocation under the lock**.

## 5. Verification

**Standalone equivalence (`topk_equiv.cpp`):** new per-shard-merge vs. a single global `partial_sort`
over the union. 20,000 randomized trials — varied shard counts, varied k, tie-heavy data, and the
fewer-than-k-hosts-per-shard case (exercises the untouched-tail handling). **0 mismatches. ASan + UBSan
clean.** Build/run: `g++ -std=c++20 -O2 -fsanitize=address,undefined topk_equiv.cpp -o t && ./t`.

**In-daemon before/after** (8-core VM, documented `cmake .. -DLINK_WITH_ABSL=ON && make`, same load:
1M hosts / 200k flows/s / 70 s / 8 threads, tracking + reporting ON, measured via the upstream
`"Traffic was calculated in:"` watchdog line which logs only when a recalc exceeds 1 s):

| metric | BEFORE (merged copy) | AFTER (per-shard top-k) |
|---|---|---|
| over-budget recalcs | 2 (1.075 s, 1.101 s) | **0** |
| peak RSS | 1489 MB | **1097 MB** (−392 MB, ~26%) |
| RSS growth over run | 78 MB | 76 MB |
| false detections | 0 | 0 |

The −392 MB peak RSS is the ~1M-entry per-second transient being removed — directly matching the perf
page-fault finding.

## 6. What this does NOT prove (honest limits)

- **Not a scaling proof.** On this fast VM the clean branch was only *marginally* over budget (2 events),
  so this demonstrates "the fix restores deadline compliance and removes a memory spike," **not** that
  more worker threads reduce recalc time in the real daemon.
- The earlier ~3.75×/~4.1× speedups were from the **standalone micro-benchmark**, not the full daemon
  under flow tracking + reporting.
- Single run each for before/after; not yet repeated for variance, and no long soak on the fixed build.

## 7. Next experiment (to actually demonstrate scaling) — staged

Run on the **fixed** daemon, tracking + reporting ON, same binary/config/rate within each comparison.
Harness: `scaling_matrix.sh` (this folder). Do NOT assume 10M must finish under 1 s — measure and report
honestly.

**Stage 1 — establish the 1M scaling curve.** Threads 1 / 2 / 4 / 8, **3 repetitions each**. This shows
whether adding sweep workers actually reduces the integrated daemon's recalc time (with reporting + flow
tracking live), and how repeatable it is.

**Stage 2 — validate at ~10M.** Only once Stage 1 is stable, same 1 / 2 / 4 / 8 × 3 matrix at ~10M
monitored hosts (the harness uses `10.0.0.0/9 + 10.128.0.0/11` = 10,485,760, and RAM-guards the row).

Monitored vs active hosts are reported separately (they are different things): the **monitored** count
(networks_list CIDR size) drives the recalc sweep cost; the **active** count (generator) drives
flow-tracking load. For 1M they are equal (1,048,576 — use that, not 1,000,000). For ~10M the generator
spans one /9 (8,388,608 active) while 10,485,760 are monitored; this is labelled, not hidden.

Metrics recorded per run (both stages): recalc **min / median / p95 / max**, **over-budget count**,
**peak RSS**, **avg/peak CPU**, and for input integrity the authoritative **parsed_flows** (daemon
Prometheus `fastnetmon_netflow_ipfix_all_protocols_total_flows` delta) vs **sent_flows** →
**flow_loss = sent − parsed** (0 ⇒ every generated flow was parsed), plus **recv_packets**, **UDP
InErrors/RcvbufErrors** and **swap in/out + OOM-kill** deltas as context. (Kernel UDP errors = 0 is *not*
proof of zero loss — `flow_loss` from the parsed counter is the real check.) Workload is deterministic
(generator seed fixed at 1), so thread-count comparisons are genuinely like-for-like. A per-row RAM
preflight refuses to start a workload that won't fit or when the box is already swapping.

**Correctness scope here is narrow:** the matrix runs with 0 attackers, so `detections` must be 0 in
every run (no false positives). Equal detection *counts* do not prove identical detection *behaviour*;
proving detection equivalence across thread counts needs a **separate** run with a fixed attacker set,
comparing the banned-IP sets / notify events — TODO, not part of this scaling matrix.

**The key result we want:** at ~10M, does 8-thread recalc time improve over 1/2/4 threads, *without*
`flow_loss` rising or `detections` changing? That is far stronger than the standalone micro-benchmark,
which does not model reporting, flow tracking, allocation, or packet ingestion.

**Honest caveat to carry:** the earlier ~3.75×/~4.1× figures were standalone; the integrated daemon may
scale differently, and 10M may legitimately exceed 1 s — if so, report the measured number, not the target.

## 8. Proposed commit message (for when we push)

See `COMMIT_MESSAGE.txt` in this folder. No AI/tooling attribution, per repo policy.
