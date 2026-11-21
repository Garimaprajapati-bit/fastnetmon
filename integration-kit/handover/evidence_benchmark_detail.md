# Step-1 container benchmark — results v2 (methodology-corrected; isolated, NOT the daemon)

Source `sweep_bench.cpp` v2. Fixes applied after review: (1) fixed-shard mode alongside production-auto;
(2) identical partition COUNT across candidates + `unordered_range` added to decompose; (3) memory column
relabelled "estimated container storage" + real VmHWM via fresh-process driver; (4) full-field,
**order-independent (commutative)** checksum; build line corrected (no `speed_counters.cpp`; needs absl).
Box: 4 cores / 15 GB (8-thread + hardware counters still require the VM). All candidates now produce an
**identical full-field checksum (`bb6cf47735df0000`)** at every thread/shard setting → EWMA equivalence proven.

## The decomposition (answers: how much is layout vs work-distribution?)

Median sweep ms, 1M, AUTO shards (= threads×16, production-equivalent), 5 repeats:
| candidate | t1 | t2 | t4 |
|---|---|---|---|
| unordered_hash (baseline) | 104 | 135 | **135** |
| unordered_range (same map, range-partitioned) | 92 | 56 | **27** |
| dense_vec_range (contiguous array) | 72 | 39 | **24** |

**At t4: baseline 135 → range-partitioned unordered_map 27 ms (≈5.0×) → dense vector 24 ms (another ≈1.14×).**

- **Work-distribution / partitioning effect (hash → range, SAME `std::unordered_map`): ≈5×.**
- **Pure container-layout effect (range map → range vector): only ≈1.15×.**

The FIXED-64-shard mode isolates it even more cleanly (same container, same shard count, same threads —
only hash vs range): `unordered_hash` t1 = 522 ms vs `unordered_range` t1 = 101 ms → **≈5× from partitioning
alone.** So most of the earlier "6.8× dense vector" win was **partitioning, not the container.**

### Why: allocation locality, not the container
Preallocating keys in order into **hash**-sharded maps scatters each shard's nodes across the heap (consecutive
keys go to different shards) → poor iteration locality, worsening with shard count, no parallel scaling.
**Range**-partitioning places each shard's nodes contiguously → good locality → scales. The container swap
(vector) only removes the remaining per-node indirection (~15%).

## Audited confirmation — 10M, independent oracle, work-balance, real peak RSS
10M hosts, AUTO shards, median sweep ms (fresh process per candidate):
| candidate | t1 | t4 | entries/part (t4) | peak RSS (VmHWM) |
|---|---|---|---|---|
| unordered_hash (baseline) | 977 | **1269** (worse) | 163840/163840 | 5676 MB |
| unordered_range (same map) | 989 | **251** (≈5.05×) | 163840/163840 | 5676 MB |
| dense_vec_range | 648 | **179** (+1.4× layout) | 163840/163840 | 5011 MB |

Audits (both 1M and 10M):
- **Correctness: `oracle_mismatch = 0`** for every candidate at 1M — each output byte-matches an INDEPENDENT
  per-key recomputation (not just cross-candidate checksum agreement). Full-field checksums identical at 10M too.
- **Work-balance: identical** — hash and range both have *exactly* equal entries per partition (16384 at 1M,
  163840 at 10M). Same scheduling. So the 5× is the memory-access pattern, NOT uneven work distribution.
- **Real memory: `unordered_range` has the SAME peak RSS as `unordered_hash`** (5676 MB) — range-partitioning
  buys ≈5× at **zero** memory cost. `dense_vec_range` is ~665 MB lower.

So the decomposition holds at the target scale: **partitioning ≈5× (both sizes), container layout ≈1.3–1.4× more.**

## Revised assessment (this overturns the earlier recommendation)
- The scaling problem is **largely the production hash-sharding's allocation locality**, NOT the `unordered_map`
  container per se. The biggest lever is the **partitioning / preallocation layout**, which could be addressed
  far less invasively than a container swap — e.g. range-partitioning, or a **per-shard node arena/pool** that
  gives locality while KEEPING hash-sharding's load balance.
- **Tradeoff to evaluate:** hash-sharding was presumably chosen for even load distribution; range-partitioning
  concentrates contiguous IPs in one shard, which could raise **packet-path lock contention** for skewed
  traffic. The sweep load stays even (N/P per shard) but the ingest side may not. Not free.
- `dense_vec_range` remains the fastest absolute and lowest memory, but its marginal gain over a well-laid-out
  map is small, and it carries the range-sharding architecture + CIDR assumptions.
- `absl flat_hash_map` stays dominated → ABSL dependency not justified.

## Honest limits (unchanged, plus new)
- Still the ISOLATED sweep (no ingest/draw/detection/tracking competition). Layout signal, not an integrated result.
- The locality effect is **allocator-dependent** (glibc here; the daemon's build/allocator may differ) — needs
  integrated confirmation before any claim.
- 4-core box: 8-thread point + hardware cache/IPC counters need the VM. 10M re-run with fixed methodology pending.
- `est_store_MB` is estimated container storage (hardcoded per-slot bytes), NOT measured RSS; real peak RSS via
  the fresh-process driver (`run_bench.sh`, VmHWM line).
- `open_addr` is benchmark-grade (no rehash/erase/concurrency); needs full review before it is a real candidate.

## Remaining Step-1 items before the Step-2 decision
8-core VM run: full 1/2/4/8 × {auto, fixed} at 1M and **10M (5–7 repeats, report spread)**, per-candidate
peak RSS (fresh process), and perf cache-miss/IPC (core counters work on the VM). Then decide Step 2 —
with the new framing that the primary lever looks like **partitioning/allocation-locality**, not a container swap.
