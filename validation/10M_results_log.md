# 10M integrated recalc — measured results log (live)

Container: 4-core Xeon @2.1GHz, 15 GB RAM. Integrated real daemon, hosts pre-allocated by the
daemon's own `subnet_vectors_allocator` from the configured networks (`10.0.0.0/9 + 10.128.0.0/11` =
10,485,760 hosts). `recalc_ms` is the real per-second `recalculate_speed()` duration (FNM_BENCH log).
Budget = 1000 ms. All runs: ban matched to `10.0.0.0/8`, 0 per-host WARNs.

## Architecture facts established (by reading + runs)
- Per-host counters live in `ipv4_host_counters` (hash-sharded `abstract_subnet_counters_t`),
  pre-allocated one entry per configured host at startup — the parallel-sweep target is correct.
- Per host, the sweep touches THREE scattered maps: `counter_map` (iterated), `average_speed_map[key]`
  (second lookup), and — with conntrack ON — `SubnetVectorMapFlow` via `increment_flow_counters`.
- **`increment_flow_counters` takes a single global `flow_counter_mutex` per host** → with parallel
  workers, all serialize on one lock. Prime scaling killer.
- Detection callback per host: patricia lookup + `subnet_to_host_groups` find + `we_should_ban…`.
  With 1–2 networks and no host groups these trees are tiny/cache-hot (minor).

## BASELINE — ban ON, conntrack ON (all production costs)
| threads | recalc_ms | speedup |
|---|---|---|
| 1 | 6020.6 | 1.00× |
| 2 | 4662.5 | 1.29× |
| 4 | 4253.4 | 1.42× |

Verdict: 4–6× over budget AND near-flat scaling (1.42× at 4 cores). This is the real problem to solve.

## Root-cause established (profiling by config toggles + probes)
- **Pure CPU probe** (fixed work/thread): wall 554→520→527 ms t1/t2/t4 ⇒ the container has 4 genuine
  parallel cores; pure compute scales ~4×. So poor sweep scaling is the CODE, not the box.
- **Pure sweep integrated** (ban OFF + conntrack OFF, old node-based map): t1 3504 → t4 3139 = **1.12×**.
  The node-based `unordered_map` sweep is memory-LATENCY bound (random heap node + 2nd map lookup per
  host) → does not scale with cores.
- **Layout probe** (real Counter + EWMA, isolated): node-based MAP t1 6896 ms; **DENSE contiguous
  t1 993 ms (7× faster) and scales t1→t4 3.73×**. Contiguous = sequential, prefetchable, bandwidth
  bound → scales.
- **conntrack ON** adds a single global `flow_counter_mutex` locked per host → serialises the parallel
  sweep (separate fix: shard that map+mutex).

## ROOT CAUSE of non-scaling — the sharding hash collapses (the decisive bug)
Per-thread CPU sampling of the real daemon during a sweep showed **only 1 of 4 worker threads busy**
(the process used 1.00 CPU) even though a 4-thread CPU burn uses 3.96 cores here and the isolated
sweep scales 3.55×. Cause: `shard_for()` used `std::hash<uint32_t>` (identity on libstdc++) then
`% num_shards`. Monitored IPs are **network-order** addresses from a few customer prefixes, so their
low bits (which a small modulo selects) are the constant leading octet — e.g. every `10.x.x.x` host
has `key % 64 == 10`. **All ~10M hosts collapse onto ONE shard; 63 shards stay empty.** One worker
sweeps everything; the others grab empty shards and exit. This is why the ORIGINAL parallel feature
never scaled either (it measured ~1.4× — one busy core plus noise). The daemon even warned about it
("ntoh required: raw big-endian keys collapse onto a handful of partitions").

## FIX 2 — avalanche the hash before the modulo (shard_for)
`shard_index()` now runs the key hash through a MurmurHash3-style finaliser before `% num_shards`, so
structured network-order keys spread evenly across shards. Deterministic and applied identically by
ingest/sweep/queries, so per-key results are unchanged. Equivalence seq==parallel still byte-identical.

## RESULT after FIX 1+2 — integrated, ban ON, conntrack OFF, 10,485,760 hosts (whole recalculate_speed)
| threads | recalc_ms (min) | speedup | within 1s budget |
|---|---|---|---|
| 1 | 1426 | 1.00× | no (single-threaded) |
| 2 | 728 | **1.96×** | YES |
| 4 | 390 | **3.66×** | YES |

Near-linear (1.96× / 3.66×) and within budget at >=2 workers, on a 4-core box. Sweep-only (excluding
the rest of recalculate_speed): t1 1047 ms -> t4 282 ms = **3.71×**, with per-thread CPU confirming 4
busy workers (was 1). The definitive 1/2/4/8 near-linear run belongs on the 8-core VM (the 4-core box
oversubscribes at 4 workers + daemon threads); harness provided.

## RESULT after FIX 1+2+3 — conntrack ON (default), ban ON, 10,485,760 hosts
| threads | recalc_ms (min) | speedup | within 1s budget |
|---|---|---|---|
| 1 | 2527 | 1.00× | no |
| 2 | 1289 | 1.96× | no (just over) |
| 4 | 796 | **3.17×** | YES |
Without FIX 3 the global flow mutex would re-serialise the sweep (~1.4× cap). Both configs now scale
near-linearly and meet the 1s budget at 4 workers on a 4-core box; more headroom on 8 cores.

## CORRECTNESS (all three fixes in place)
- Equivalence seq(1-shard) vs parallel(64-shard/8-thread): **byte-identical**, 0 mismatches (K=200k).
- ThreadSanitizer on the counter class (30 sweeps + writers + readers, 6M callbacks): **0 warnings**.
- ASan/UBSan: clean, no leaks.
- **Detection parity** (real daemon, real attacker traffic, threads=1 OFF vs threads=4 ON): both detect
  the identical 30 attacker IPs — `IDENTICAL DETECTION -> REGRESSION PASS`.
- Whole daemon + all unit/perf tests build clean.

## FIX 3 — sharded connection-tracking store (fastnetmon_types.hpp + fastnetmon*.cpp)
`SubnetVectorMapFlow` was one std::map under one global `flow_counter_mutex`, locked once per host by
the sweep and by every packet-path flow update. Replaced with `sharded_flow_counters_t` (256 maps +
256 mutexes, keyed by a mixed hash of the host-order IP). Same per-IP semantics; different hosts use
different locks, so the parallel sweep's flow reads and packet threads' writes rarely contend.

## FIX 1 — dense contiguous storage (implemented in abstract_subnet_counters.hpp)
Replaced per-shard `{unordered_map counter_map; unordered_map average_speed_map}` with dense parallel
vectors `keys[] / counters[] / average[]` + a `key->slot` index (used only for ingest/point queries).
The sweep is now a contiguous linear scan; the per-host 2nd hash lookup is gone. Correctness: dense
equivalence seq==parallel **byte-identical** (0 mismatches), **ASan/UBSan clean**. Build: daemon + all
tests compile. Integrated scaling results below.
