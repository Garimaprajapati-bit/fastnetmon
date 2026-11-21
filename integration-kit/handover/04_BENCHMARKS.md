# Benchmark results + honest scaling analysis

Two kinds of measurement: (A) the **integrated daemon** sweep under real NetFlow load (what production
sees), and (B) an **isolated** micro-benchmark that decomposes *why* it scales as it does. Both are
reproducible (`04` tools in this folder + the integration kit). 8-thread / 10M figures come from the VM run.

## A. Integrated daemon — recalc over-budget at 1M hosts (measured, 4-core container)
Real daemon, 1M monitored hosts, 200k flows/s, tracking + detection ON. Watchdog logs
"Traffic was calculated in:" only when a recalc exceeds the 1-second budget.

| build | over-budget recalcs | peak RSS |
|---|---|---|
| sharded, before the top-k fix | 2 (1.07 s, 1.10 s) | 1489 MB |
| sharded, **with** the top-k fix | **0** | **1097 MB** (−392 MB) |

The top-k reporting fix removes a ~1M-entry/second allocation and the over-budget events, with detection
output unchanged.

## B. Isolated sweep scaling + decomposition (measured, 4-core; 8-thread [VM])
`sweep_bench.cpp` runs the real `subnet_counter_t` + verbatim EWMA over the real sharded iteration, with an
**independent oracle** (`oracle_mismatch=0`) and **identical per-partition work-balance** across variants.

Median sweep ms, 1M hosts, production-auto shards (= threads×16):
| variant | t1 | t2 | t4 |
|---|---|---|---|
| `unordered_hash` (current production layout) | 104 | 135 | 135 (no gain) |
| `unordered_range` (same container, range-partitioned) | 92 | 56 | **27** |
| `dense_vec_range` (contiguous array) | 72 | 39 | 24 |

10M hosts (same shape): `unordered_hash` 977→1269 (worse with threads); `unordered_range` 989→**251**;
`dense_vec_range` 648→**179**. Correctness identical (checksums + oracle match at both sizes).

### What this means (evidence-based, not asserted)
- **The current hash-sharded `std::unordered_map` does not scale with threads** — its node-per-host heap
  layout scatters each shard's nodes, so more workers don't help (memory-access bound). This is a
  **pre-existing property of the storage**, reproduced here in isolation; it is not caused by the
  parallelization machinery (lock/thread overhead measured <1% by `perf`).
- **The dominant lever is the data-partitioning layout, not the container type**: range-partitioning the
  *same* `unordered_map` recovers ~5× (at zero extra memory, oracle-clean, identical work-balance); swapping
  to a contiguous vector adds only ~1.3× more.
- Therefore near-linear scaling is **not achievable by the current sharding alone** — it is memory-bound.

## C. Honest limitation + the scoped next lever (NOT in this deliverable)
The measured next step to materially improve scaling is **range-based preallocation retaining
`std::unordered_map`**. It is deliberately **not** included here because it has two untested risks that must
be evaluated before it touches the detection hot path:
1. **Ingest-side lock contention** — hash-sharding spreads active hosts across shards evenly; range-sharding
   concentrates a hot prefix into one shard's lock. Needs a uniform/skewed/hot-prefix ingest test.
2. **Integrated-daemon + allocator dependence** — the isolated win must reproduce in the full daemon.

Shipping it under-validated would risk the "preserve production behaviour" requirement, so it is documented
as the evidence-backed next experiment, not presented as done.

## Reproduce
```bash
FNM_SRC=/root/fastnetmon THREADS=1,2,4,8 REP=7 bash run_bench.sh      # isolated, 1M + 10M, per-candidate RSS + perf
# integrated over-budget: see 02_BUILD_AND_TEST.md section D(4)
```
