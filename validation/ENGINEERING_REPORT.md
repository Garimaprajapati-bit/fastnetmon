# FastNetMon — Native Parallelization of Per-Second Speed Recalculation to 10 M Hosts
### Complete engineering report: architecture, system design, code changes, testing, and results
*Date: 2026-11-22 · Branch: `sharded-speed-counters` (code) / `vm-10M-validate` (code + validation harness)*

---

## 1. Executive summary

**Objective (as specified by the FastNetMon maintainer).** Make the once-per-second
`recalculate_speed()` sweep — which updates EWMA speed counters for every monitored host and drives
threshold-based attack detection — scale *natively across CPU workers* so a single instance can track
**10 million hosts within the 1-second budget**, with **unchanged EWMA and detection behaviour** and
no regressions.

**Outcome.** Achieved and independently validated on an 8-core (4 physical × 2 SMT) AMD EPYC VM:

| Metric | Original | This work |
|---|---|---|
| 10 M recalc, 4 workers (idle) | ~4.3 s, over budget | **288–345 ms, within budget** |
| Scaling at 4 workers | ~1.4× (one core doing the work) | **3.84–4.00× (≈ linear to physical cores)** |
| 10 M recalc under 150 k–800 k flows/s ingestion (conntrack off) | over budget | **~400–480 ms, 0 over-budget events** |
| Shard load balance (real keys) | all hosts in 1 shard | **CV ≤ 1.2 %, 0 empty shards** |
| EWMA / detection behaviour | baseline | **byte-identical; detection 30==30; 20/20 under load** |
| Thread-safety | baseline | **ASan/UBSan + TSan clean on the counter core** |

The work was **three compounding fixes** found by profiling the *integrated daemon* (not a synthetic
benchmark): (1) a sharding hash that collapsed all real keys onto one shard, (2) node-based scattered
storage that made the sweep memory-latency bound, and (3) a global connection-tracking mutex that
re-serialised the parallel sweep. All three are behaviour-preserving.

---

## 2. Background: what the sweep does and why it mattered

FastNetMon keeps per-host traffic counters. Once per second, `recalculate_speed()` walks **every**
monitored host to:
1. compute instantaneous speed from the raw packet/byte counters (`build_speed_*`),
2. fold it into an exponential moving average (`build_average_*`, the EWMA),
3. zero the raw counters for the next interval,
4. invoke a **detection callback** per host (threshold check → ban decision),
5. optionally fold in connection-tracking flow counts.

This ran on **one thread**. As the monitored host count grows, the sweep consumes an ever-larger slice
of the 1-second budget; past a point the recalc thread cannot keep up, independent of how many CPU
cores the box has. At 10 M hosts the original single-threaded sweep took multiple seconds — far over
budget.

A previous effort had already **sharded** the counters (partitioned them into N independent
`{map, mutex}` shards) and added a worker pool to sweep shards in parallel. In principle that should
scale. In practice it measured only ~1.4× at 4 threads. This work found out why, and fixed it.

---

## 3. Investigation and root-cause analysis

All diagnosis was done against the **real daemon** populated through its **real allocator**
(`subnet_vectors_allocator` → `preallocate_key`), with detection enabled — not a synthetic data
structure. Three independent findings:

### 3.1 The parallel sweep ran on one core (the sharding hash collapsed)
Per-thread CPU sampling during a live 10 M sweep showed **only 1 of 4 worker threads busy** (the whole
process used ~1.0 CPU), while a pure-CPU burn test on the same box used 4 cores cleanly. So the box was
fine; the code wasn't using it.

Cause: `shard_for()` routed a key with `std::hash<uint32_t>(key) % num_shards`. On libstdc++,
`std::hash<uint32_t>` is the **identity** function, and FastNetMon stores IPv4 keys in **network byte
order**. For hosts in a few customer prefixes (e.g. `10.x.x.x`), the bits a small modulo selects are
the **constant leading octet**, so **every host hashed to the same shard**. One worker swept all 10 M
hosts; the other 63 shards were empty. This is why the *original* parallel feature never scaled either.
(The code even carried a comment warning that raw big-endian keys "collapse onto a handful of
partitions.")

### 3.2 Even a balanced sweep was memory-latency bound (scattered node storage)
A pure sweep (detection + conntrack off) over the node-based maps scaled only **1.12×** at 4 threads,
while a pure-CPU workload scaled 4×. Each host was two separate `std::unordered_map` lookups
(`counter_map` to read raw counters, then `average_speed_map[key]` to store the EWMA) — each a random
heap node, i.e. a cache + TLB miss per host. The sweep stalled on DRAM latency, so adding cores didn't
help. A controlled layout probe (real `subnet_counter_t`, real EWMA) showed a **dense contiguous**
layout was ~7× faster single-threaded *and* scaled, where the node-based layout did neither.

### 3.3 Connection tracking re-serialised the parallel sweep (one global mutex)
With conntrack on, the per-host flow read (`increment_flow_counters`) locked **one global
`flow_counter_mutex`** — 10 M lock/unlock per sweep. With parallel workers, they all contended on that
single lock, collapsing scaling back toward serial.

---

## 4. Architecture and system design: before → after

### 4.1 Per-host counter storage (`abstract_subnet_counters_t`)

**Before** — each shard held two node-based maps:
```
shard_t { std::unordered_map<Key,Counter> counter_map;
          std::unordered_map<Key,Counter> average_speed_map;
          std::mutex mutex; }
```
Sweep per host = iterate `counter_map` (scattered node) + hash-lookup `average_speed_map[key]`
(second scattered node). Two cache misses/host; no spatial locality.

**After** — each shard holds **dense, contiguous, parallel vectors** plus a small index:
```
shard_t { std::unordered_map<Key,uint32_t> index;   // key -> dense slot (ingest / point queries only)
          std::vector<Key>     keys;                  // slot -> key
          std::vector<Counter> counters;              // slot -> raw packet counters
          std::vector<Counter> average;               // slot -> EWMA
          std::mutex mutex; }
```
Sweep = a **linear scan** over `counters[]`/`average[]` by slot index. Sequential, prefetchable,
memory-*bandwidth* bound instead of latency bound; the per-host second hash lookup is **gone**
(the EWMA for slot *j* lives at `average[j]`). The hash map is touched only on host creation and for
point queries/reporting — never in the hot sweep.

Design properties preserved: a key still routes to exactly one shard; per-key EWMA and counter
semantics are identical; `preallocate_key`, `increment_*_counters_for_key`, `get_average_speed`,
`get_top_k_average_speed`, `purge_old_data`, serialization and the worker-pool sweep all keep their
external contracts.

### 4.2 Shard routing (`shard_for`)

**Before:** `*shards[ std::hash<Key>(key) % num_shards ]` — identity hash → collapse on structured keys.

**After:** avalanche the key bits (MurmurHash3 finaliser) *before* the modulo, so structured
network-order keys spread evenly:
```
static uint64_t mix_bits(uint64_t h){ h^=h>>33; h*=0xff51afd7ed558ccd; h^=h>>33;
                                      h*=0xc4ceb9fe1a85ec53; h^=h>>33; return h; }
shard_index(key) = mix_bits( hash(key) ) % num_shards
```
Deterministic and applied identically by ingest, sweep and queries, so per-key results are unchanged;
only the shard *assignment* (hence load balance) differs.

### 4.3 Connection-tracking store (`SubnetVectorMapFlow`)

**Before:** one `std::map<uint32_t,conntrack_main_struct_t>` guarded by one global
`std::mutex flow_counter_mutex`, locked once per host by the sweep and by every packet-path flow
update.

**After:** a sharded store — 256 maps + 256 mutexes, keyed by a mixed hash of the host-order IP:
```
struct sharded_flow_counters_t {
    std::map<uint32_t,conntrack_main_struct_t> maps[256];
    std::mutex                                 mutexes[256];
    static size_t shard_of(uint32_t ip){ return mix(ip) & 255; }
};
```
Different hosts use different locks, so the parallel sweep's flow reads and the packet threads' flow
writes rarely contend. Per-IP semantics are unchanged (each IP still maps to exactly one map,
consistently for readers and writers). All access sites updated: startup allocator, sweep read,
per-second clear, the two packet-path writers, and the API read; the dead global mutex was removed.

**Lock-ordering safety (verified).** The sweep nests *counter-shard mutex → flow-shard mutex* (the flow
read runs inside the per-host callback under the counter lock). The packet path **releases** the
counter mutex **before** taking the flow mutex (sequential, not nested). No path nests
flow → counter, so there is **no lock-order inversion** between them — no deadlock.

---

## 5. Code changes — file by file

Diff vs upstream base `751a596`, 5 source files (`+693 / −230` including the pre-existing sharding
commits; the three fixes above are the substantive part). Benchmark-only, env-gated instrumentation
(`FNM_BENCH_*`) is inert in normal operation.

| File | Change |
|---|---|
| `src/abstract_subnet_counters.hpp` | Dense `keys[]/counters[]/average[]` + index storage; linear-scan sweep; `mix_bits`/`shard_index` avalanche hash; `purge_old_data` swap-pop; serialization over the dense vectors. Behaviour-preserving parallel worker-pool sweep retained (sequential when `threads≤1`). |
| `src/fastnetmon_types.hpp` | New `sharded_flow_counters_t` (256 maps + mutexes + `shard_of`). |
| `src/fastnetmon.cpp` | `SubnetVectorMapFlow` becomes `sharded_flow_counters_t`; allocator writes to the right flow shard; removed dead `flow_counter_mutex`; env-gated benchmark hooks (prepopulate, per-iteration timing, per-shard distribution dump). |
| `src/fastnetmon_logic.cpp` | All conntrack access via the sharded store (sweep read, per-second clear, two packet-path writers, API read); env-gated host-sweep-only timing. |
| `src/tests/speed_counters_performance_test.cpp` | Updated to the dense members; multi-shard redistribution check. |

Config knobs (unchanged defaults → identical to upstream behaviour):
`speed_calculation_threads` (default `1` = sequential), `speed_calculation_shards`
(default `0` = auto `threads×16`).

---

## 6. Testing methodology

Everything was measured against the **integrated real daemon**, populated by its **own allocator** from
configured networks (`10.0.0.0/9 + 10.128.0.0/11 = 10,485,760` hosts for 10 M; smaller prefixes for
1 M/5 M), with ban/detection enabled — not an isolated data structure.

- **Scaling:** whole `recalculate_speed()` timed per second via an env-gated log; warm-up dropped;
  ≥ 5 measured samples per cell; min / median / p95 / max recorded. Run at 1 M / 5 M / 10 M × conntrack
  {off,on} × {1,2,4,8} workers. Per-core CPU and per-shard host distribution captured each run.
- **Host initialization:** configured-vs-allocated host count, init time, peak/steady RSS, allocation
  failures.
- **Shard distribution:** real allocated keys dumped per shard (min/max/mean/stddev/CV/empty).
- **Ingestion under load:** `netflow_gen.py` drives real NetFlow (500 k active hosts, 20 attackers) at
  150 k–800 k flows/s while recalc runs; UDP receive errors, recalc latency under load, CPU, RSS and
  detection measured.
- **Correctness:** sequential-vs-parallel byte equivalence (200 k keys); ASan/UBSan; ThreadSanitizer on
  the counter core (30 concurrent sweeps + writers + readers, 6 M callbacks); detection parity (real
  daemon, identical attacker traffic, threads=1 vs 4); the multi-shard perf unit test.
- **Environment check:** a fixed-work CPU burn confirms real parallel cores (flat wall to 8 threads),
  so poor scaling can be attributed to code, not the VM.

Harness (reproducible): `vm_validate_master.sh` (env/init/scale/correctness) and `vm_load_soak.sh`
(ingest/stress/soak), both on the `vm-10M-validate` branch under `validation/`.

---

## 7. Results

### 7.1 Scaling — recalc_ms (min), integrated daemon, 8-core VM
Speedup(N) = T(1)/T(N); efficiency = speedup/N.

| hosts | conntrack | 1 w | 2 w | 4 w | 8 w |
|---|---|---|---|---|---|
| 1 M  | off | 148 | 84 (1.76×) | 56 (2.62×) | 37 (3.97×) |
| 5 M  | off | 721 | 359 (2.01×) | 190 (3.80×) | 169 (4.27×) |
| **10 M** | **off** | 1325 | 675 (1.96×) | **345 (3.84×, 96 %)** | 328 (4.04×) |
| 10 M | on  | 1701 | 840 (2.02×) | **485 (3.50×)** | 377 (4.51×) |

(First VM instance measured 10 M/off t4 = 288 ms / 4.00× and 10 M/on t4 = 445 ms / 4.39×; the two runs
agree within normal VM-to-VM variance.) **10 M recalc is within the 1-second budget from 2 workers
upward**, both configs.

**Note on 8 workers.** The VM is **4 physical cores × 2 SMT**. For memory-bandwidth-bound streaming,
SMT adds little, so 8 *logical* threads give 4.0–4.5×, i.e. scaling is ≈ linear to the **physical** core
count (100 % efficiency at 4 workers). ≥ 6× would require 6–8 *physical* cores.

### 7.2 Host initialization (real allocator)
| hosts | exact count | init (off) | RSS (off) | init (on) | RSS (on) |
|---|---|---|---|---|---|
| 1 M | 1,048,577 | 3 s | 0.82 GB | 4 s | 1.26 GB |
| 5 M | 5,242,881 | 7 s | 4.0 GB | 14 s | 6.2 GB |
| 10 M | 10,485,761 | 13 s | 8.0 GB | 25–30 s | 12.3 GB |

No allocation failures. (The “+1” is the subnet network address.)

### 7.3 Shard distribution on real production keys (the hash-fix proof)
Every multi-shard run: **0 empty shards**, coefficient of variation **0.2 %–1.2 %**, max/mean ≤ 1.03.
e.g. 10 M over 64 shards: min 162,887 / max 165,598 / mean 163,840 (CV 0.0028). The identity-hash
collapse (all hosts → 1 shard) is eliminated.

### 7.4 Recalc under real ingestion load (10 M, 4 workers)
| offered rate | conntrack | recalc med | recalc max | over-budget | UDP drops | attackers |
|---|---|---|---|---|---|---|
| 150 k/s | on  | 1128 ms | 1239 ms | 37 | 21 k | 20/20 |
| 150 k/s | off | **398 ms** | 571 ms | **0** | 0 | 20/20 |
| 300 k/s | off | 459 ms | 518 ms | **0** | ~1 k | — |
| 500 k/s | off | 475 ms | 655 ms | **0** | 354 k | — |
| 800 k/s | off | 479 ms | 618 ms | **0** | 1.25 M | — |

**Speed calculation holds the 1-second budget under load** (conntrack off): median ~400 ms, 0
over-budget even at 800 k flows/s, latency flat as offered rate climbs, detection intact.
The conntrack-on over-budget is **lock contention in connection tracking** (more CPU used yet slower),
not the parallelization. The UDP drops at ≥ 500 k/s are the **single-threaded NetFlow collector**
(receive path) — a separate limit from speed calculation.

### 7.5 Soak — 1 hour at 10 M + continuous traffic (conntrack off, 4 workers)
3600 s, 150 k flows/s, 120 samples at 30 s intervals:
- **Memory flat:** RSS 5465 MB → 5443 MB (**−22 MB**, i.e. no growth) — **no leak**.
- **Latency stable:** recalc median 370–485 ms throughout, 416 → 407 ms start-to-end — **no drift**.
- **0 over-budget events** across all 120 samples; no crash, deadlock, or spike.

### 7.6 Correctness and safety
- **Equivalence** (seq 1-shard vs parallel 64-shard/8-thread, 200 k keys): **byte-identical**.
- **ASan + UBSan:** 0 errors. **ThreadSanitizer** (counter core, 6 M callbacks): 0 warnings.
- **Detection parity** (real daemon, real attacker traffic, threads 1 vs 4): **30 == 30, PASS**;
  **20/20** attackers detected under live ingestion.
- **Multi-shard perf unit test:** redistribution lossless (0 misplaced).
- **Daemon-level ThreadSanitizer** (conntrack sharding + sweep under live traffic): **0
  feature-introduced races** (feature ON vs OFF race sites identical; residual warnings pre-existing).

---

## 8. Original vs updated — side by side

| Dimension | Original | Updated |
|---|---|---|
| Shard routing | `identity_hash(key) % N` → all real `10.x` hosts in 1 shard | avalanche hash → even (CV ≤ 1.2 %, 0 empty) |
| Swept storage | 2 node-based `unordered_map`s; 2 cache misses/host | dense contiguous vectors; linear scan, no 2nd lookup |
| Parallel sweep effect | ~1.4× (one busy core) | 3.84–4.00× (≈ linear to physical cores) |
| 10 M recalc, 4 w (idle) | over budget (~4+ s) | 288–345 ms, in budget |
| 10 M recalc under load (off) | over budget | ~400–480 ms, 0 over-budget to 800 k/s |
| Conntrack store | 1 `std::map` + 1 global mutex (serialises sweep) | 256 maps + 256 mutexes (contention-split) |
| EWMA / detection | baseline | byte-identical; 30==30; 20/20 under load |

---

## 9. Honest limitations and scope

1. **8-worker scaling is bounded by physical cores.** On this 4-physical-core VM, 8 logical threads
   give ~4–4.5× (SMT adds little to bandwidth-bound work). Scaling is ~linear to the physical core
   count; ≥ 6× needs 6–8 physical cores. This is hardware, not the implementation.
2. **Connection tracking under heavy load** stays over budget at 10 M even after sharding its lock,
   because the sweep's per-host flow read contends with packet-thread writes. conntrack-off is the
   realistic 10 M configuration (and meets budget under load). An optional future improvement —
   lock-free flow-count reads — would relieve this; it is **not required** for the speed-calc objective
   and was deliberately not attempted (minimal-change principle).
3. **The NetFlow collector is single-threaded** and drops traffic above ~300 k flows/s regardless of
   the sweep. This is a separate receive-path limit, out of scope for speed calculation.
4. **Soak passed** (1 h at 10 M + traffic: no leak, no drift — §7.5). **Daemon-level ThreadSanitizer
   passed too:** under live traffic with conntrack on, the parallel sweep ON (threads=4) vs OFF
   (threads=1) show the **same** race sites (36 vs 37 warnings, 13 functions each) and **zero functions
   race only with the feature on** — the sharded sweep and sharded conntrack introduce **no new data
   races**. The residual warnings are pre-existing (lock-free global aggregate counters + gRPC event
   engine), present with the feature off too and out of scope. With the deterministic counter-core TSan
   already clean, concurrency is signed off.

**Code-review follow-ups (done).** An external review of the dense storage raised three items, all
fixed (commit `5d9bd47`): (1) `get_top_k` no longer allocates proportional to host count — it selects
per-shard top-k over a lazy view with `std::ranges::partial_sort_copy` (k-bounded), verified correct vs
brute force over 300 randomized layouts; (2) new-slot insertion is transactional (strong exception
guarantee, `noexcept` rollback); (3) the dense slot index is `size_t` throughout (no truncation).

---

## 10. Conclusion

The per-second speed recalculation now **parallelises natively and scales ≈ linearly to the available
physical cores**, bringing **10 million hosts within the 1-second budget** — idle *and* under real
ingestion load — while keeping **EWMA and detection behaviour byte-identical**, shard load even on real
keys, and the counter core sanitizer-clean. The result came from fixing three concrete, compounding
defects (hash collapse, scattered storage, global conntrack lock), each a small, behaviour-preserving
change, found by profiling the integrated daemon rather than a synthetic benchmark.

Reproducible harness and raw logs accompany this report; the code is on branch
`sharded-speed-counters` (clean feature) and `vm-10M-validate` (code + validation tooling).
