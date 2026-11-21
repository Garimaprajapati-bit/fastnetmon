# FastNetMon — native parallelization of per-host speed calculation
## What changed, why, and code-review report

Branch: `sharded-speed-counters` (base: upstream `751a596`). **Off by default** — with no config change the
behaviour is byte-identical to upstream.

## 1. Objective
Parallelize the once-per-second `recalculate_speed()` sweep (which computes per-host EWMA speeds and runs
detection) so it can use multiple cores, to support large host counts — **without changing the EWMA math,
detection logic, counter semantics, packet-processing path, or host lifecycle.**

## 2. The change, file by file

### `src/abstract_subnet_counters.hpp` (core)
Replaced the single `{counter_map, average_speed_map, global mutex}` with **N independent shards**, each a
`{counter_map, average_speed_map, mutex}`. A key is routed to exactly one shard by `hash(key) % N`, so a
host's live counters, its average and its EWMA always stay together — only the *lock* is partitioned.
- Per-packet increments lock **only the key's shard** (was: one global lock for every packet).
- `recalculate_speed()` sweeps shards: **sequential when `recalc_threads<=1` (identical to original)**, else
  worker threads pull shards off an atomic counter. Each shard is processed under its own lock; shards are
  independent so the per-shard body is data-race-free.
- Worker-spawn hardening: if `std::thread` creation throws (RLIMIT_NPROC/stack exhaustion), we keep the
  workers already started (or sweep inline if none) instead of letting a joinable-thread vector call
  `std::terminate()`. Every shard is still processed exactly once.
- Exception safety: a throwing callback in a worker is captured and re-thrown after join, matching the
  sequential path's propagation.
- `get_top_k_average_speed()` **fix**: the first sharded version concatenated every host across all shards
  into one vector each call (O(hosts) allocation per second → page-fault storm + held all shard locks). Now
  it computes each shard's top-k under its own lock into a reused k-sized buffer, then merges
  (≤ shards·k entries). Global top-k of a union == top-k of the concatenation of per-shard top-k, so the
  result is unchanged. Allocation dropped from O(hosts) to O(shards·k).

### `src/fastnetmon.cpp`
- Parse `speed_calculation_threads` (default 1) and `speed_calculation_shards` (default 0 = auto = threads×16
  when threads>1, else 1). `configure_sharding()` is called **once at startup** inside
  `load_configuration_file()` (which has no reload path), before any counter is touched.
- Preallocation routes through the sharded `preallocate_key()`.

### `src/fastnetmon_logic.cpp`
- The two `recalculate_speed()` call sites pass the existing detection/flow callbacks unchanged.
- Flow-counter folding during the parallel sweep uses `std::atomic` accumulators (order-independent sum;
  identical single-threaded result).
- Accessors updated to the sharded API (`get_top_k_average_speed`, `modify_counter_for_key`, etc.).

### `src/fastnetmon.conf`
- Documented, commented-out `speed_calculation_threads` / `speed_calculation_shards` (opt-in).

### `src/tests/`
- `speed_counters_performance_test.cpp` reworked for multi-shard sources.
- `speed_calculation_scaling_test.cpp` added (scaling micro-benchmark).

## 3. Code-review findings (self-review)

**Behaviour preservation**
- Off by default: `threads=1, shards=0 → 1 shard, sequential` — the original code path.
- EWMA unchanged: the per-key math is byte-identical (verified against an independent oracle; see tests).
- `get_top_k` output unchanged (proven equivalent; detection does not use it — display only).

**Concurrency safety (verified, not just argued)**
- Shards are independent (separate maps + mutex); the parallel sweep touches disjoint shards per worker.
- Per-packet increments and the sweep share the *same* shard mutex → serialized per shard; different shards
  run concurrently on disjoint data.
- The detection callback (`speed_calculation_callback_local_ipv4/ipv6`) only touches shared structures that
  are **all internally mutex-guarded**: `blackhole_ban_list_t` (every method locks `structure_mutex`),
  `packet_buckets_storage_t` (every method locks `packet_buckets_map_mutex`); patricia-tree lookups are
  read-only at runtime; `log4cpp` is thread-safe. It does **not** execute ban/mitigation actions inline.
- The flow callback takes the existing global `flow_counter_mutex` (serialized; correct, if not parallel).
- Confirmed by ThreadSanitizer (container test clean; daemon-under-load TSan in the VM harness).

**Lifecycle / reload safety**
- `configure_sharding()` resets storage and is documented "startup only". Verified `load_configuration_file()`
  is called exactly once (`main`), with no SIGHUP/reload path — so counters are never reset mid-run.

**Allocation review**
- The only O(hosts)/second allocation introduced (the top-k concat) was found and removed.
- `get_sorted_average_speed()` (O(n) copy) is called only on **network** counters (a handful of subnets),
  not host counters — not a host-scale path.

**Minor / accepted**
- Worker threads are created and joined per sweep rather than pooled. Measured overhead is negligible
  (<0.2% of CPU in profiling); kept for simplicity and correctness. A persistent pool is a possible future
  micro-optimization, not a correctness issue.

## 4. Honest scope note
This delivers *native parallelization* (opt-in, correct, safe). Measured scaling of the sweep is **sub-linear**
and plateaus (memory-bandwidth bound); see `03_TEST_RESULTS.md` and `04_BENCHMARKS.md` for the measured
numbers and the evidence-based analysis of the remaining ceiling and the next lever (data-partitioning
locality). That ceiling is a hardware/data-layout property, not a defect in the parallelization.
