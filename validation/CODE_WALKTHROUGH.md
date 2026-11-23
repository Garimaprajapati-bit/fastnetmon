# Annotated code walkthrough — why each change is written the way it is
*Read alongside `src/abstract_subnet_counters.hpp`, `src/fastnetmon_types.hpp`, `src/fastnetmon_logic.cpp`, `src/fastnetmon.cpp`.*

This explains the reasoning behind every substantive line so you can defend the design in review.

---

## Part A — `abstract_subnet_counters_t` (the per-host counter container)

### A.1 The shard struct — why dense parallel vectors (lines ~53–63)
```cpp
struct shard_t {
    using slot_index_t = size_t;
    std::unordered_map<T, slot_index_t> index;  // key -> dense slot
    std::vector<T> keys;                         // slot -> key
    std::vector<Counter> counters;               // slot -> current packet counters
    std::vector<Counter> average;                // slot -> average speed
    std::mutex mutex;
};
```
**Why this shape.** The old shard held two `std::unordered_map`s (`counter_map`, `average_speed_map`).
A map stores each element in a separately-allocated node scattered across the heap, so the once-per-
second sweep that touches *every* host paid a cache miss (often a TLB miss) per host — the sweep was
**memory-latency bound** and didn't scale with cores. Here the live data is three **contiguous**
vectors indexed by a dense integer `slot`. The sweep walks them linearly → the hardware prefetcher
streams them → **memory-bandwidth bound**, which *does* scale with cores. The `index` map still exists
but is touched only on host creation and for point lookups (ingest / queries), never in the hot sweep.

**Why three parallel vectors (SoA) and not one `vector<struct{key,counter,average}>`.** Either works;
parallel arrays keep each stream maximally dense and let the sweep touch only `counters`/`average`
without pulling `keys` into cache. `keys` is needed only to pass the key to the detection callback.

**Why `slot_index_t = size_t`.** The slot is an index into vectors whose size is `size_t`. Using
`size_t` (not `uint32_t`) means `slot = keys.size()` never truncates — correctness over the (tiny)
memory saving of a 32-bit value. Used consistently by the map, inserts and purge.

### A.2 Transactional insert — why nested try/rollback (lines ~70–83)
```cpp
inline slot_index_t append_slot(const T& key, const Counter& init_counter, const Counter& init_average) {
    slot_index_t slot = keys.size();
    keys.push_back(key);
    try { counters.push_back(init_counter);
        try { average.push_back(init_average);
            try { index.emplace(key, slot); }
            catch(...){ average.pop_back(); counters.pop_back(); keys.pop_back(); throw; }
        } catch(...){ counters.pop_back(); keys.pop_back(); throw; }
    } catch(...){ keys.pop_back(); throw; }
    return slot;
}
```
**The invariant.** The four containers must always agree: `keys.size()==counters.size()==average.size()`
and `index[key]` must point at a real slot. If we naively did four appends and the 3rd threw
`std::bad_alloc`, we'd be left with `keys` one longer than `counters`, or an `index` entry pointing at a
non-existent slot — silent corruption that a later sweep would dereference out of bounds.
**Why this structure works.** `std::vector::push_back` has the *strong* guarantee (on throw, the vector
is unchanged), and `pop_back` is `noexcept`. So each level either fully succeeds or we roll back the
levels already done, with rollbacks that cannot themselves throw. Net effect: **all-or-nothing** — the
container is never left half-updated. (In practice this only triggers under OOM, but "corrupt vs clean
failure" is exactly what exception safety is about, and the reviewer was right to require it.)

### A.3 `counter_ref` — one lookup, create-on-miss (lines ~88–93)
```cpp
inline Counter& counter_ref(const T& key) {
    auto it = index.find(key);
    if (it != index.end()) return counters[it->second];
    slot_index_t slot = append_slot(key, Counter{}, Counter{});
    return counters[slot];
}
```
Replaces the old `counter_map[key]`. One hash lookup; on miss, a transactional append. The returned
reference is valid only until the next insert (a `push_back` may reallocate), which is why the doc
comment says callers use it under the lock and don't hold it across inserts — every caller does exactly
that (lock, get ref, mutate, unlock).

### A.4 `shard_for` / `mix_bits` — the hash-collapse fix (lines ~110–133)
```cpp
static inline uint64_t mix_bits(uint64_t h){ h^=h>>33; h*=0xff51afd7ed558ccd; h^=h>>33;
                                             h*=0xc4ceb9fe1a85ec53; h^=h>>33; return h; }
size_t shard_index(const T& key) const { return mix_bits((uint64_t)Hasher{}(key)) % num_shards; }
```
**The bug this fixes.** `Hasher` is `std::hash<uint32_t>`, which on libstdc++ is the **identity**
function. IPv4 keys are stored in **network byte order**, so for hosts in a handful of customer prefixes
(e.g. everything in `10.0.0.0/8`) the low-order bits that `% num_shards` selects are the *constant*
leading octet. Result: `hash(key) % N` sent **every** host to the same shard; the other shards stayed
empty; the "parallel" sweep ran one busy worker. `mix_bits` is the MurmurHash3 64-bit finaliser — it
avalanches the bits so structured keys spread evenly (measured CV ≤ 1.2 %, 0 empty shards at 10 M).
**Why it's behaviour-safe.** The mapping is deterministic and used by ingest, sweep and queries alike,
so a key always lands in the same shard; only *which* shard changes. Per-key counters/EWMA are
unaffected — proven by the byte-identical equivalence test.

### A.5 `recalculate_speed` — the sweep (worker pool unchanged; body is now a linear scan)
The per-shard body iterates `for (j in 0..keys.size())`, reads `counters[j]`, computes the speed,
invokes the optional callbacks, writes `average[j]` in place, then zeroes `counters[j]`. Compared to the
old body it drops the second map lookup (`average_speed_map[key]`) entirely — `average[j]` is O(1).
The threading is unchanged: sequential when `recalc_threads<=1` (byte-identical to upstream), otherwise
workers pull shards off an atomic counter; a worker-spawn failure degrades to inline sweeping; the first
callback exception is captured and rethrown after join (so a throwing detection callback behaves exactly
as in the single-threaded path).

### A.6 `get_top_k_average_speed` — bounded reporting (the review fix)
```cpp
auto pair_view = std::views::iota(size_t{0}, s->keys.size())
               | std::views::transform([&s](size_t j){ return std::make_pair(s->keys[j], s->average[j]); });
auto res = std::ranges::partial_sort_copy(pair_view, shard_top, comparator);
```
**Why a view.** `get_top_k` must return the k most severe hosts. The dense storage keeps keys and
averages in *separate* arrays, so there is no ready-made range of `pair<key,average>` to sort. The first
version materialised a temporary vector of *all* hosts in the shard to form those pairs — that is the
O(hosts) allocation the reviewer flagged (and it happened under the shard lock). The fix forms the pairs
**lazily** via `views::iota | views::transform`: the pair for slot `j` is produced only when the
algorithm dereferences `j`, so no full vector is built. `partial_sort_copy` keeps only a **k-sized**
internal buffer. Allocation is now bounded by `k` (and `shards*k` for the final merge), never by host
count.
**Why `std::ranges::partial_sort_copy` and not the classic `std::partial_sort_copy`.** The classic
algorithm can't consume a C++20 view iterator (it deduces the iterator's `value_type` as `void` and
fails to compile). The `ranges::` version is designed for views. It returns `{in, out}`; `out` is the end
of what was written, which we use to copy only the real winners into the merge buffer.
**Why it's correct.** Global top-k over all shards == top-k of the concatenation of per-shard top-k
(any globally-top element is top within its own shard). Verified directly against a brute-force full
sort over 300 randomized multi-shard layouts: 0 mismatches.

### A.7 `serialize` / `purge_old_data` — kept working with the new layout
`serialize` writes the three vectors and, on load, rebuilds `index` from `keys`. `purge_old_data` removes
an expired host by swap-with-last + `pop_back` on all three vectors and fixes the moved key's `index`
entry (now a `size_t`). Both are cold paths; the daemon never persists these counters (serialization is
used only by the developer perf test).

---

## Part B — Connection tracking (`sharded_flow_counters_t`)

### B.1 The struct (`src/fastnetmon_types.hpp`, lines ~506–515)
```cpp
static const size_t FLOW_COUNTER_SHARD_COUNT = 256;      // power of two
struct sharded_flow_counters_t {
    map_of_vector_counters_for_flow_t maps[256];
    std::mutex                        mutexes[256];
    static size_t shard_of(uint32_t ip){ return mix(ip) & 255; }
};
```
**Why.** Connection tracking kept all per-host flow records in one `std::map` guarded by one global
`flow_counter_mutex`, which the sweep locked once **per host** (10 M lock/unlock per sweep) and every
packet-path flow update also took. Once the sweep is parallel, that single lock re-serialises every
worker. Sharding into 256 independent `{map, mutex}` lets different hosts use different locks, so the
sweep's flow reads and the packet threads' flow writes rarely contend. `& 255` because the count is a
power of two (cheap mask instead of modulo). Keyed by the **host-order** IP (`shard_of` mixes it), the
same value used everywhere this store is accessed, so each IP consistently maps to one map.

### B.2 Lock ordering — why there's no deadlock
The sweep holds the **counter**-shard mutex and, inside the per-host detection/flow callback, takes a
**flow**-shard mutex → order is *counter → flow*. The packet path takes the counter mutex to bump
counters and **releases it** before separately taking the flow mutex to record the flow
(`fastnetmon_logic.cpp`: counter update ~2504, flow update ~2512 — sequential, never nested). Because no
code path ever takes *flow then counter*, the two lock classes have a single global order and cannot
deadlock. (Confirmed by the daemon-level ThreadSanitizer run: 0 feature-introduced races.)

### B.3 The access sites (all updated, `fastnetmon_logic.cpp` / `fastnetmon.cpp`)
Startup allocator writes `maps[shard_of(ip)][ip]`; the sweep read, the per-second `clear()` (loops all
256), the two packet-path writers, and the API read all index the right shard under its own mutex. The
now-dead global `flow_counter_mutex` was removed (the only remaining definitions are independent stubs
inside two fuzz harnesses, unrelated to the daemon global).

---

## Part C — Config and defaults (why this is safe to merge)
`speed_calculation_threads` defaults to `1` and `speed_calculation_shards` to `0` (→ 1 shard when
threads=1). So out of the box the code takes the **sequential, single-shard** path — byte-identical to
upstream. Parallelism is strictly opt-in. The benchmark-only hooks (`FNM_BENCH_*`) are guarded by
`getenv` and do nothing unless those env vars are set.
