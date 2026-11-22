# Stage 1 — Implementation assessment (pre-VM), 2026-11-22

Independent review of the current local implementation before any VM testing. Nothing is assumed
correct from prior benchmarks. Verified-here vs must-verify-on-VM is marked throughout.

## Repository state
- Branch `sharded-speed-counters`, base = upstream `751a596`.
- **Committed** (5 commits) — the original parallelization:
  `e2c8471` parallelise per-host speed calc across sharded counters; `8e354dd` scaling benchmark;
  `912e1a6`/`b83686d` perf test for sharded counters; `f161784` harden worker-spawn failure.
- **Uncommitted, local only** (the 10M work) — 5 files, +256/−137 vs HEAD:
  `abstract_subnet_counters.hpp`, `fastnetmon_types.hpp`, `fastnetmon.cpp`, `fastnetmon_logic.cpp`,
  `tests/speed_counters_performance_test.cpp`. Nothing committed/pushed.
- Benchmark-only, env-gated hooks exist in the daemon (`FNM_BENCH_PREPOP`, `FNM_BENCH_LOG`) — inert
  unless the env vars are set.

## 1. Dense counter storage (`abstract_subnet_counters_t`)
Per shard: `std::unordered_map<T,uint32_t> index` (key→slot) + parallel vectors `keys[] / counters[]
/ average[]`. Replaces the former two node-based maps (`counter_map` + `average_speed_map`).
- Ingest/`modify`/`preallocate_key` go through `counter_ref(key)` → O(1) index lookup, append on miss.
- Sweep is a contiguous linear scan over `counters[]`/`average[]`; the former per-host second hash
  lookup (`average_speed_map[key]`) is gone.
- `get_average_speed`/`get_top_k`/`get_sorted`/`get_all` iterate the vectors.
- **Checks (verified here):** reference from `counter_ref` is used immediately under the shard lock
  (no held reference across inserts); the sweep inserts nothing (no vector reallocation mid-sweep);
  `preallocate_key` is idempotent. **Risk to watch on VM:** `purge_old_data` now uses swap-with-last +
  pop and rewrites the moved key's index entry — correctness verified by reading, but it must be
  exercised on VM (enable automatic cleanup) since production uses it.

## 2. Shard routing hash (`shard_for` / `mix_bits`)
The defect: `std::hash<uint32_t>` is identity on libstdc++, and monitored IPs are **network-order**,
so `hash(key) % N` put every `10.x.x.x` host in one shard (low bits = constant leading octet). Fix:
a MurmurHash3 finaliser mixes the bits before the modulo.
- **Verified here:** per-thread CPU during a real-daemon sweep went from 1 busy worker → 4; sweep-only
  1047→282 ms (3.71×) at 10.49M. **Must verify on VM (§6 requirement):** dump the *actual* per-shard
  host counts from the production allocator (not just aggregate) to confirm even distribution of real
  keys, at 1M/5M/10M.

## 3. Sharded connection tracking (`sharded_flow_counters_t`)
256 `std::map`s + 256 `std::mutex`es, keyed by `mix_bits(host-order IP) & 255`, replacing one global
map + one global `flow_counter_mutex`. All sites updated: allocator init, sweep read
(`increment_flow_counters`), per-second `zeroify_all_flow_counters` (clears all 256), the two
packet-path writers, and the API read (`get_element_from_map_of_flow_counters`). The dead
`flow_counter_mutex` was removed.
- **Lock-ordering (verified here):** the sweep nests counter-shard-mutex → flow-shard-mutex (flow read
  happens inside the per-host callback under the counter lock). The packet path releases the counter
  mutex *before* taking the flow mutex (fastnetmon_logic.cpp:2502/2504 then 2512/2517 — sequential,
  not nested). No path nests flow→counter, so **no lock-order inversion / deadlock** between them.
- **Must verify on VM:** TSan on the daemon under real traffic (concurrent packet writers + sweep
  readers across the 256 flow shards), and that conntrack-based flow counts/detection are unchanged.

## 4. Synchronization / init / clear / API (verified here, re-verify under load on VM)
- Per-packet increments lock only the key's counter shard. Sweep locks each shard once.
- The detection callback runs under the counter shard lock and, only for over-threshold hosts,
  acquires ban-list / packet-bucket / whitelist locks — **this nesting pre-dates these changes** (the
  callback was always invoked under the shard lock). Flag for the VM deadlock/TSan/soak gate, but it is
  not introduced here.
- Startup allocation path: `subnet_vectors_allocator` → `preallocate_key` (counter) +
  `SubnetVectorMapFlow.maps[shard][ip]` (conntrack). This is the real production population path the
  VM tests must use (not a synthetic injector).

## 5. Tests present
- `equiv_concurrency_test.cpp` — seq vs parallel byte-equivalence + TSan/ASan concurrency target.
- `speed_counters_performance_test.cpp` — multi-shard redistribution correctness + timing.
- `speed_calculation_scaling_test.cpp` — isolated scaling.
- Handover harnesses: `integrated_recalc_bench.sh` (real-daemon recalc_ms at N workers),
  `regression_detection.sh` (detection parity), `sweep_only_probe.sh` (sweep-only + per-thread CPU),
  `validate_on_vm.sh` (8-core acceptance matrix + soak).

## Results already obtained on the 4-core dev box (to be re-established cleanly on the VM)
- Integrated recalc_ms min, 10,485,760 hosts: conntrack OFF 1426→728(1.96×)→390(3.66×);
  conntrack ON 2527→1289(1.96×)→796(3.17×). Within budget at ≥2/≥4 workers respectively.
- Correctness: byte-identical equivalence; TSan clean (counter class); ASan/UBSan clean; detection
  parity 30==30; perf test 0 misplaced.
- Caveat: 4-core box oversubscribes at 4 workers + daemon threads → noisy medians; `min` used as the
  compute floor. 8 cores needed for the clean 1/2/4/8 matrix and the 8-worker ≥6× target.

## Open items the VM must settle (not yet proven)
1. Real per-shard distribution of production keys at 1M/5M/10M (hash fix on real data).
2. Clean 1/2/4/8 scaling + efficiency with p95, both configs, both ban states.
3. 10M init: allocation success, time, peak/steady RSS, no allocation failures.
4. Real packet ingestion concurrent with recalc (loss, contention, detection accuracy).
5. Stress + soak (1–4 h): no leak, no deadlock, no latency drift.
6. Daemon TSan on the conntrack sharding under traffic; `purge_old_data` under cleanup.
7. Original (committed, pre-dense) vs modified before/after comparison on identical VM config.

## Verdict (Stage 1)
The implementation is internally consistent, the three fixes are logically sound, no new lock-order
inversion was found, and the dev-box evidence is strong. It is **ready to enter VM validation**. It is
**not** yet proven at 10M under real ingestion/stress/soak, nor at 8 workers — that is the VM's job.
