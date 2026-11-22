# 8-core VM acceptance — Stages 2,4-6,10(core) PASSED (executed 2026-11-22)

VM: AMD EPYC-Milan (KVM), **4 physical cores × 2 SMT = 8 logical**, 30 GB RAM, Ubuntu 26.04, gcc-15,
no cgroup CPU limit (burn test flat to 8 threads). Integrated real daemon; hosts populated by the
daemon's own allocator from configured networks. recalc_ms = whole recalculate_speed(), min of settled
samples (p50/p95/max also recorded). Budget = 1000 ms.

## Recalc scalability — speedup(N)=T(1)/T(N), efficiency=speedup/N
| scale | conntrack | t1 ms | t2 | t4 | t8 |
|---|---|---|---|---|---|
| 1M  | off | 107  | 56 (1.90×) | 30 (3.58×) | 27 (3.97×) |
| 1M  | on  | 126  | 72 (1.76×) | 47 (2.69×) | 35 (3.56×) |
| 5M  | off | 536  | 284 (1.89×) | 145 (3.69×) | 125 (4.27×) |
| 5M  | on  | 668  | 368 (1.82×) | 216 (3.08×) | 172 (3.87×) |
| **10M** | **off** | 1150 | 582 (1.98×) | **288 (4.00×, 100%)** | 247 (4.65×) |
| **10M** | **on**  | 1955 | 748 (2.61×) | **445 (4.39×)** | 341 (5.73×) |

## Host initialization (real allocator)
| scale | hosts (exact) | init off | RSS off | init on | RSS on |
|---|---|---|---|---|---|
| 1M  | 1,048,577  | 3 s | 0.82 GB | 4 s | 1.26 GB |
| 5M  | 5,242,881  | 7 s | 4.0 GB  | 14 s | 6.2 GB |
| 10M | 10,485,761 | 13 s | 8.0 GB | 25 s | 12.3 GB |
No allocation failures. (The +1 over the round number is the subnet's network address.)

## Shard distribution on REAL production keys (the hash-fix proof)
Every multi-shard run: **empty_shards=0**, coefficient of variation **0.2%–1.2%**, max/mean ≤ 1.03.
e.g. 10M/64 shards: min 162,887 / max 165,598 / mean 163,840, cv 0.0028. The identity-hash collapse
(all hosts → 1 shard) is gone; load is near-perfectly even.

## Correctness
- Equivalence seq vs parallel: **byte-identical** (0 mismatches, 200k keys).
- ASan/UBSan: **0 errors**. Counter-class ThreadSanitizer: **0 warnings**.
- Detection parity (real daemon, real attacker traffic, threads=1 vs 4): **30 == 30, REGRESSION PASS**.

## Verdict vs acceptance criteria
| criterion | target | result |
|---|---|---|
| 10M init | success | ✅ exact counts, 13–25 s |
| 4-worker speedup | ≥ 3× | ✅ **4.00× (off), 4.39× (on)** |
| 10M recalc | < 1 s | ✅ **288 ms (off), 445 ms (on)** at 4w; in budget from 2w |
| seq/parallel equivalence | 100% | ✅ byte-identical |
| data races | none | ✅ counter-class TSan clean (daemon-level conntrack TSan under traffic: pending Stage 7/soak) |
| detection behaviour | preserved | ✅ 30==30 |
| 8-worker speedup | ideally ≥ 6× | ⚠️ 4.65× (off) / 5.73× (on) — **this VM has 4 PHYSICAL cores**; SMT adds little to memory-bound streaming. Scaling is ~linear to the 4 physical cores (100% efficiency at t4); ≥6× needs 6–8 physical cores. |

**Core objective ACHIEVED:** 10M hosts, native parallel recalc, within the 1 s budget, ~linear to the
physical core count, EWMA + detection unchanged, even shard load, sanitizer-clean counter core.

## Still to run (robustness, Stages 7-9 + full 10)
Real traffic ingestion concurrent with recalc; stress; 1–4 h soak (leak/drift); daemon-level TSan on
the conntrack sharding under live packet writers; optional original-vs-modified A/B.
