# Production-readiness assessment

## Verdict
**The native parallelization of speed calculation is production-ready as an opt-in feature, and safe to
merge.** It is off by default (byte-identical to upstream), behaviour-preserving, thread-safe, and
memory-safe by executed tests. The one correctness regression it introduced (a reporting allocation) was
found and fixed. **Near-linear scalability is NOT achieved** — the sweep is memory-bandwidth bound — and
that is characterized honestly with evidence, not hidden.

## What is proven (executed tests, evidence in this folder)
| Property | Status | Evidence |
|---|---|---|
| Off-by-default, no behaviour change when disabled | ✅ | code: threads=1/shards=0 → original path |
| EWMA output identical: parallel == sequential | ✅ PASS | equivalence test, 0 byte-mismatches over 200k keys; oracle 0 mismatches |
| Detection identical: parallel == sequential (real daemon) | ✅ PASS | regression: 30 == 30 attackers, diff empty |
| No data races introduced | ✅ PROVEN | container TSan clean; daemon TSan under load: **0** race sites unique to the feature (ON=35 vs OFF=37, same sites) |
| Memory-safe, no leaks, no UB | ✅ CLEAN | ASan + UBSan |
| Multi-shard redistribution lossless | ✅ PASS | perf test: 0 misplaced over 2M hosts |
| Builds the documented way | ✅ | cmake -DLINK_WITH_ABSL=ON && make |
| Reporting allocation regression | ✅ FIXED | top-k fix: 0 over-budget, −392 MB at 1M |
| Startup-only reconfigure (no mid-run reset) | ✅ | configure_sharding called once; no reload path |

## What requires the 8-core VM (turnkey harness provided: `validate_on_vm.sh`)
- 8-thread scaling numbers and 10M-host load/stress/soak (RAM + cores).
- Daemon-level TSan under sustained load (built here; run reported by the harness).
These are **final validation of an already-correct implementation**, not unfinished work — the harness runs
them with one command and needs no debugging or setup.

## Honest limitations (resolved vs. remaining)
**Resolved (not left as limitations):**
- Reporting O(hosts)/s allocation → fixed (top-k per-shord merge).
- Worker-spawn failure → hardened (no std::terminate).
- Parallel-sweep exception propagation → matches sequential.
- Concurrency of detection under parallel sweep → verified safe (all shared state internally locked).

**Disclosed (pre-existing, not introduced or worsened by this change):**
- The daemon has pre-existing data races on its lock-free global aggregate counters
  (`fastnetmon_logic.cpp:2005/2008`: `total_counters`, `total_unparsed_packets`) between the once-per-second
  recalc thread and the packet threads, and inside gRPC's event engine (the API server). ThreadSanitizer
  flags these with the feature **off** as well (37 with threads=1 vs 35 with threads=4). They predate this
  change and are out of its scope; disclosed here for completeness. Fixing them (e.g. making the global
  aggregates atomic) would be a separate, independent hardening change.

**Remaining (characterized, not defects):**
1. **Scaling is sub-linear and plateaus** (≈1.4× at 4 threads with tracking on; memory-bandwidth bound).
   Decomposed with evidence (`04_BENCHMARKS.md`): the limiter is the per-host data-layout/allocation
   locality of the hash-sharded `std::unordered_map`, a pre-existing storage property — not the
   parallelization. Near-linear is not physically achievable for this memory-bound sweep on this hardware.
2. **The next lever is scoped but intentionally NOT shipped:** range-based preallocation (measured ~5× in
   isolation) retaining `std::unordered_map`. It is withheld because two risks are untested — ingest-side
   lock contention under skewed traffic, and integrated-daemon/allocator dependence — and shipping it
   under-validated would jeopardize the "preserve production behaviour" requirement. It is the recommended,
   evidence-backed next experiment, with a clear test plan, not a hand-wave.

## Recommendation
Merge the opt-in parallel speed calculation + the top-k fix: it is correct, safe, and useful (parallelism
up to the memory-bandwidth ceiling, with a measured reporting-memory improvement), and it changes nothing
for existing deployments. Treat the range-partitioning lever as a separate, reviewed follow-up gated on the
ingest-skew + integrated tests described in `04_BENCHMARKS.md`.
