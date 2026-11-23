# Annotated code walkthrough (v2 — review-corrected)
*Read alongside `src/abstract_subnet_counters.hpp`, `src/fastnetmon_types.hpp`, `src/fastnetmon_logic.cpp`, `src/fastnetmon.cpp`.*

This version corrects the inaccuracies raised in review: the rollback is fixed (not just reworded),
the hash claim is backed by measured key values + histograms, and the exception / complexity /
lock-ordering / flow-reset claims are stated precisely. Each claim below says what is *proven* vs
*argued*, and names the test that backs it.

---

## Part A — `abstract_subnet_counters_t` (per-host counter container)

### A.1 Dense parallel vectors — what it does, and what the evidence is (lines ~55–63)
```cpp
struct shard_t {
    using slot_index_t = size_t;
    std::unordered_map<T, slot_index_t> index;  // key -> dense slot (ingest / point queries only)
    std::vector<T> keys; std::vector<Counter> counters; std::vector<Counter> average;
    std::mutex mutex;
};
```
**What changed.** The old shard held two `std::unordered_map`s; the sweep read `counter_map` and then
did a second lookup `average_speed_map[key]` per host. Node-based maps place each element in a
separately-allocated heap node, so iterating them is a scattered, pointer-chasing access pattern.
The new layout keeps the live data in three **contiguous** vectors indexed by a dense `slot`; the sweep
is a linear scan and the per-host *second* lookup is gone (`average[slot]` is a direct index).
**Precise claim.** Contiguous storage improves cache/TLB locality and removes a per-host hash lookup.
It does **not**, on its own, "guarantee bandwidth-bound scaling" — the actual scaling is an empirical
result, established by the integrated benchmarks (10 M recalc: ~1.4× before → ~3.8–4.0× at 4 workers,
within the 1 s budget; see the engineering report). SoA (three arrays) over AoS (one array of a struct)
keeps each stream dense and lets the sweep avoid pulling `keys` into cache except to pass to callbacks.

### A.2 Transactional insert — the corrected rollback (lines ~65–83)
```cpp
inline slot_index_t append_slot(const T& key, const Counter& ic, const Counter& ia) {
    slot_index_t slot = keys.size();
    int done = 0;                                  // steps fully completed
    try {
        keys.push_back(key);  ++done;              // 1
        counters.push_back(ic); ++done;            // 2
        average.push_back(ia);  ++done;            // 3
        index.emplace(key, slot); ++done;          // 4
    } catch (...) {
        if (done >= 3) average.pop_back();
        if (done >= 2) counters.pop_back();
        if (done >= 1) keys.pop_back();
        throw;
    }
    return slot;
}
```
**The bug this replaced (found in review).** The first version used *nested* `try/catch`, one per step.
That was wrong: an inner `catch` that fully rolled back then `throw;`d — and because it sat inside the
enclosing `try`, the outer `catch` fired too and popped the *same* vectors again → `pop_back()` on an
already-empty vector, which is undefined behaviour.
**Why the new form is correct.** A single `catch` rolls back **exactly** the steps counted in `done`,
so each `pop_back` runs at most once and never on an empty vector. `std::vector::push_back` has the
strong guarantee (on throw the vector is unchanged, so the throwing step is never rolled back) and
`pop_back` is `noexcept`. Net: the four containers (`keys/counters/average/index`) are always equal in
size with the index pointing at real slots — all-or-nothing.
**Proven by** `fault_insert_test.cpp`: forces a throw at each step (via a `Counter` whose copy-ctor
throws) and asserts the four containers stay consistent and a later insert still works. Compiled with
`-D_GLIBCXX_ASSERTIONS` it **PASSES on this version and ABORTS on the old nested version**
(`pop_back: Assertion '!this->empty()' failed`) — so the test genuinely distinguishes the two.

### A.3 `counter_ref` and the lock invariant (lines ~85–93)
Replaces `counter_map[key]`: one hash lookup, create-on-miss via `append_slot`. **Invariant:** the
returned `Counter&` is valid only until the next insert on this shard (a `push_back` may reallocate), so
callers must use it under the shard lock and not hold it across inserts. **Verified at every call site:**
the four callers (`increment_incoming/outgoing_counters_for_key`, `modify_counter_for_key`, and the
packet-counter array overloads) all take the shard lock, get the ref, mutate, and release within the
same scope — none stores the reference or inserts again while holding it.

### A.4 `shard_for` / `mix_bits` — the collapse, demonstrated with real values (lines ~110–133)
```cpp
static uint64_t mix_bits(uint64_t h){ h^=h>>33; h*=0xff51afd7ed558ccd; h^=h>>33;
                                      h*=0xc4ceb9fe1a85ec53; h^=h>>33; return h; }
size_t shard_index(const T& key) const { return mix_bits((uint64_t)Hasher{}(key)) % num_shards; }
```
**How the key is stored.** `src/fastnetmon.cpp` `subnet_vectors_allocator` builds the key as
`stored = hton(host_order_ip)` — **network byte order** — and calls `preallocate_key(stored)`.
**Measured evidence** (`hash_distribution_evidence.cpp`, using that exact construction):

| address | `stored_key` (uint32) | `std::hash(stored) % 64` | `mix_bits % 64` |
|---|---|---|---|
| 10.0.0.0 | 0x0000000A = 10 | **10** | 13 |
| 10.0.0.1 | 0x0100000A | **10** | 19 |
| 10.0.0.2 | 0x0200000A | **10** | 30 |
| 10.0.0.3 | 0x0300000A | **10** | 45 |

The first octet (10) sits in the **low byte** of the network-order `uint32`, and `std::hash<uint32_t>`
is identity on libstdc++, so `stored % 64` is **always 10** for the whole `10.0.0.0/8`. Histogram over
10,485,760 hosts across 64 shards:

| hash | shards used | min | max | mean | CV | max/mean |
|---|---|---|---|---|---|---|
| identity | **1 / 64** | 0 | 10,485,760 | 163,840 | 7.94 | **64.0** |
| mix_bits | **64 / 64** | 162,887 | 165,598 | 163,840 | **0.0028** | 1.0 |

That is the before/after, on production-shaped keys. The production allocator confirms the same on the
live daemon (per-shard dump: CV ≤ 1.2 %, 0 empty at 10 M).
**Behaviour.** The hash changes only *which shard* a key maps to; per-key arithmetic (EWMA/counters) is
intended to be unchanged, and the equivalence test (A below) provides the evidence (0 byte-mismatches).

### A.5 `recalculate_speed` — sweep + the precise exception claim
The per-shard body is a linear scan: read `counters[j]`, compute speed, run the optional callbacks,
write `average[j]` in place, zero `counters[j]`. Threading is unchanged (sequential when
`recalc_threads<=1`; otherwise workers pull shards off an atomic).
**Exception semantics — stated precisely.** If a callback throws in a worker, the first exception is
captured and **re-thrown to the caller after all workers join** — so it does not escape a worker thread
and call `std::terminate`. This is *not* the same as the sequential path: with multiple workers, other
workers may still process their shards, more hosts may be updated, and callback order differs. The
guarantee is **propagation to the caller without process termination**, not identical partial state or
callback order.

### A.6 `get_top_k_average_speed` — bounded *memory*, not bounded *work*
```cpp
auto pair_view = std::views::iota(size_t{0}, s->keys.size())
               | std::views::transform([&s](size_t j){ return std::make_pair(s->keys[j], s->average[j]); });
auto res = std::ranges::partial_sort_copy(pair_view, shard_top, comparator);
```
**Memory complexity (the fix).** The previous version materialised a temporary vector of *all* hosts in
the shard (O(hosts)) under the lock. The lazy `iota|transform` view produces each pair only on
dereference, so `partial_sort_copy` holds only a **k-sized** buffer. Scratch memory is now O(k) per
shard and O(shards·k) for the merge — never O(hosts).
**Time complexity (unchanged, stated explicitly).** It is **not** O(k) in time: the algorithm still
traverses the whole shard, so comparison work is ~**O(N log k)**. The fix reduces *allocation*, not the
scan. `std::ranges::partial_sort_copy` (not the classic `std::partial_sort_copy`) is required because
the classic one cannot consume a C++20 view iterator (it deduces `value_type` as `void`).
**Correctness proven by** `topk_header_test.cpp`: the real `get_top_k_average_speed` vs a brute-force
full sort, 300 randomized multi-shard layouts, 0 mismatches.

### A.7 `serialize` / `purge_old_data` — tested separately
`serialize` writes the three vectors and, on load, **rebuilds `index`** from `keys` (size_t loop). The
equivalence/round-trip is exercised by the perf test's dump/load path. `purge_old_data` removes an
expired host by **swap-with-last + pop_back** on all three vectors and fixes the moved key's `index`
entry; its correctness (no stale/duplicate index, sizes stay equal) is covered by the perf test's
redistribution check (0 misplaced). These are cold paths; the daemon never persists these counters.

---

## Part B — Connection tracking (`sharded_flow_counters_t`)

### B.1 The struct (`src/fastnetmon_types.hpp`, lines ~506–515) — contention is measured, not assumed
256 `{std::map, std::mutex}` keyed by `mix(host-order ip) & 255`, replacing one global map + one global
`flow_counter_mutex`. The claim that "different hosts rarely contend" rests on the same even
distribution shown in A.4 (the mix spreads hosts across the 256 flow shards) **and** on the integrated
evidence: with conntrack on and the sweep parallel, recalc scales (e.g. 2.61× at 2 workers / 10 M) and
the 1-hour soak + stress ran with no contention-induced stall. The residual over-budget under heavy
load is the conntrack *work* (per-second map rebuild), characterised in the report — not lock
starvation.

### B.2 Reset semantics + lock ordering — stated narrowly, with tests
**Reset boundary changed (disclosed).** The old reset held the single global mutex across the whole
`clear()`, so it was **global-atomic** w.r.t. writers (writers blocked for the entire clear). The
sharded reset clears each map under its **own** mutex, so it is **per-shard-atomic**: a write to an
already-cleared shard lands in the next interval while a write to a not-yet-cleared shard is included in
the one being reset. **What is guaranteed** (proven by `flow_reset_concurrency_test.cpp`: 6 writers ×
2 M increments racing a continuous resetter): **conservation** — every unit written is drained by
exactly one reset, `issued == drained` (12,000,000 == 12,000,000), with **TSan clean (0 warnings)**. So
no write is lost, torn, or double-counted; only *which* 1-second bucket a boundary flow falls in can
differ by sub-millisecond timing. For flow-*rate* detection this is immaterial, and the integrated
detection-parity test (identical attackers, and 20/20 under live ingestion with conntrack on) shows
detection is unaffected.
**Lock ordering — the verified paths (not a global proof).** Enumerated every flow-mutex acquisition
(`fastnetmon_logic.cpp` 1880 sweep-read, 2202 reset, 2583/2629 packet writers) and checked nesting with
the counter-shard mutex:
- The **only** simultaneous hold of a counter and a flow mutex is the **sweep** (`process_shard` holds
  the counter mutex and the flow lambda at line 1969 calls `increment_flow_counters` → flow mutex):
  order **counter → flow**.
- The **packet path** takes the counter mutex and *releases* it (2502/2504) **before** taking the flow
  mutex (2512/2517) — sequential, never nested.
- The **reset** (2202) and the **API read** hold only a flow mutex (the API read also holds a *bucket*
  mutex, not a counter mutex).
- The **detection callback** touches **no** flow mutex (grep: 0).
No path takes **flow → counter**, so the two lock classes this work introduces/changes cannot invert.
**Caveats, honestly:** this verifies the counter/flow lock classes, not an exhaustive all-mutex deadlock
proof; the detection callback's pre-existing nesting under the counter mutex (ban-list / packet-bucket
mutexes) is **unchanged** by this work. ThreadSanitizer detects **data races, not deadlocks** — the
daemon-level TSan result (0 feature-introduced races) is evidence of race-freedom, and the 1 h soak +
stress running without a hang is *empirical* (not formal) evidence of no deadlock in practice.

---

## Part C — Defaults (what "byte-identical when off" means, precisely)
`speed_calculation_threads` defaults to `1` and `speed_calculation_shards` to `0` (→ 1 shard). On that
path the sweep is sequential over a single shard. **What was compared:** the equivalence test feeds a
sequential 1-shard container and a parallel 64-shard/8-thread container identical traffic and compares
the **per-key average-speed `subnet_counter_t` byte-for-byte** over 200,000 keys (0 mismatches), and the
real daemon's detected-attacker set is identical at threads=1 vs 4. This is behavioural equivalence of
the *outputs and per-key state*, not a claim that the new storage is literally the upstream container.

---

## Test index (all runnable; in `validation/` on `vm-10M-validate`)
| Claim | Test | Result |
|---|---|---|
| Insert rollback is correct | `fault_insert_test.cpp` (`-D_GLIBCXX_ASSERTIONS`) | PASS (old version aborts) |
| Hash collapse / fix on real keys | `hash_distribution_evidence.cpp` | identity 1/64 → mix 64/64, CV 0.0028 |
| Top-k correct + k-bounded memory | `topk_header_test.cpp` | 300 trials, 0 mismatches |
| Flow reset conservation + races | `flow_reset_concurrency_test.cpp` (+TSan) | issued==drained, 0 warnings |
| Seq≡parallel per-key output | `equiv_concurrency_test.cpp` (+ASan/UBSan/TSan) | 0 mismatches, sanitizer-clean |
| Detection unchanged | `regression_detection.sh` | 30==30; 20/20 under load |
