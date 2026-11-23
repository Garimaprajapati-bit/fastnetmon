# Codebase impact / regression analysis
*Did changing these files affect anything else? Enumerated below — the answer is: contained.*

## Method 1 — every call into the changed containers, across the whole codebase
All uses of `ipv4/ipv6_host_counters` and `ipv4/ipv6_network_counters` (grep of `src/*.cpp`):

| Method called | Changed? | Status |
|---|---|---|
| `increment_incoming_counters_for_key` / `increment_outgoing_counters_for_key` | internals (now `counter_ref`) | ✅ same signature + semantics |
| `recalculate_speed` | body is dense linear scan; threading unchanged | ✅ same signature; seq path byte-identical |
| `modify_counter_for_key` | uses `counter_ref` | ✅ same signature |
| `preallocate_key` | uses transactional `append_slot` | ✅ same signature |
| `get_top_k_average_speed` | bounded view-based selection | ✅ same signature; top-k verified vs brute force |
| `get_sorted_average_speed` | iterates dense vectors | ✅ same signature |
| `average_speed_size` / `size` | sum over dense vectors | ✅ same |
| `configure_sharding` | unchanged | ✅ |
| `.shards` (direct) | only the perf test | ✅ perf test updated |

**Every production caller uses the public API, and every public method kept its signature and
behaviour.** The parallel path is opt-in; the default (threads=1) path is byte-identical to upstream
(proven by the equivalence test: 0 mismatches over 200k keys).

## Method 2 — stale references to removed/renamed internals
- **`.counter_map` / `.average_speed_map`** (the old map members): **0 references anywhere** outside the
  container header. Nothing in the daemon or tests reached into those members.
- **Direct shard internals** (`.shards`, `s->keys/counters/average/index`): **only** in
  `src/tests/speed_counters_performance_test.cpp`, which was updated to the dense members (it reads
  `s->keys`/`s->counters` and reports `s->counters.capacity()`); it builds and passes
  (0 misplaced on the redistribution check).

## Method 3 — the one signature I changed
`get_all_average_speed_elements(UM&)` → `get_all_average_speed_elements(std::unordered_map<T,Counter>&)`.
Grep for callers across `src/`: **none**. It is an unused accessor, so the change affects nothing.

## Method 4 — connection tracking (`SubnetVectorMapFlow`)
- All six daemon access sites (startup allocator, sweep read `increment_flow_counters`, per-second
  `zeroify_all_flow_counters`, the two packet-path writers, the API read
  `get_element_from_map_of_flow_counters`) were converted to the sharded store. Grep confirms **no
  stale, non-sharded access** remains.
- `get_element_from_map_of_flow_counters` signature changed from `map_of_vector_counters_for_flow_t&`
  to `sharded_flow_counters_t&`; its **only** caller (`process_filled_buckets_ipv4`) passes the sharded
  store — updated and correct.
- The removed global `flow_counter_mutex`: the only remaining occurrences are **independent local
  stubs** inside two fuzz harnesses (`tests/fuzz/process_netflow_packet_v5_fuzz.cpp`,
  `parse_sflow_v5_packet_fuzz.cpp`) that define their own to satisfy the linker — unrelated to the daemon
  global, unaffected.

## Build / test impact
- Full daemon builds clean (`-DLINK_WITH_ABSL=ON -DENABLE_GOBGP_SUPPORT=ON …`).
- `speed_counters_performance_test` builds and passes (0 misplaced, redistribution lossless).
- `speed_calculation_scaling_test` present and building.
- No change to public headers consumed by other modules beyond the container itself; the API server,
  BGP, capture plugins, etc. compile unchanged.

## Net assessment
The blast radius is exactly three areas and nothing more:
1. the container internals (behind an unchanged public API),
2. the connection-tracking store (all call sites updated; one internal-signature change with one
   updated caller),
3. the developer perf test (updated).

No production module outside these reaches into the changed internals, no public signature used by other
code changed meaning, and the default (sequential) behaviour is byte-identical to upstream. That is why
the change is safe to review as a self-contained unit and to keep off by default until merged.
