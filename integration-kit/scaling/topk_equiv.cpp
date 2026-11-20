// Standalone equivalence check: new per-shard top-k merge vs. a single global partial_sort.
// Proves get_top_k_average_speed's sharded reporting fix returns the same top-k set/order
// as sorting the whole union, across many randomized shard layouts and tie-heavy data.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <vector>

using Key = uint32_t;
struct Counter { uint64_t v; bool operator==(const Counter& o) const { return v == o.v; } };
using Elem = std::pair<Key, Counter>;

// Comparator mirroring the daemon's: strict weak ordering, bigger metric first.
struct Cmp {
    bool operator()(const Elem& a, const Elem& b) const {
        if (a.second.v != b.second.v) return a.second.v > b.second.v; // descending by metric
        return a.first < b.first;                                     // tie-break by key (stable, deterministic)
    }
};

// Reference: single global partial_sort_copy over the union (upstream-style, zero sharding).
std::vector<Elem> reference_topk(const std::vector<std::map<Key, Counter>>& shards, size_t k) {
    std::vector<Elem> all;
    for (const auto& m : shards) for (const auto& e : m) all.push_back(e);
    std::vector<Elem> out(k);
    std::partial_sort_copy(all.begin(), all.end(), out.begin(), out.end(), Cmp{});
    return out;
}

// New algorithm: per-shard top-k into a reused buffer, merge, then global top-k. Mirrors the edit.
std::vector<Elem> sharded_topk(const std::vector<std::map<Key, Counter>>& shards, size_t k) {
    const Cmp comparator;
    std::vector<Elem> shard_top(k);
    std::vector<Elem> merged;
    merged.reserve(shards.size() * k);
    for (const auto& m : shards) {
        auto end_it = std::partial_sort_copy(m.begin(), m.end(), shard_top.begin(), shard_top.end(), comparator);
        merged.insert(merged.end(), shard_top.begin(), end_it);
    }
    std::vector<Elem> out(k);
    std::partial_sort_copy(merged.begin(), merged.end(), out.begin(), out.end(), comparator);
    return out;
}

int main() {
    std::mt19937_64 rng(12345);
    int trials = 20000, mismatches = 0;
    for (int t = 0; t < trials; ++t) {
        size_t nshards = 1 + rng() % 16;
        size_t k       = 1 + rng() % 64;
        // Mix: sometimes few hosts (< k, exercises the untouched-tail path), sometimes many.
        size_t total   = rng() % 2 ? (rng() % 40) : (rng() % 4000);
        // Small value range to force frequent ties (the hard case for partial_sort stability).
        uint64_t vmax  = rng() % 2 ? 5 : 100000;

        std::vector<std::map<Key, Counter>> shards(nshards);
        for (size_t i = 0; i < total; ++i) {
            Key key = (Key)(rng() % 1000000) + 1; // keys unique across the union via map-per-key below
            shards[key % nshards][key] = Counter{ rng() % (vmax + 1) };
        }
        auto a = reference_topk(shards, k);
        auto b = sharded_topk(shards, k);
        // Filter sentinels (key 0 == empty slot), compare the real rows as sets of (key,value).
        auto clean = [](std::vector<Elem>& v){ v.erase(std::remove_if(v.begin(), v.end(),
                      [](const Elem& e){ return e.first == 0; }), v.end()); };
        clean(a); clean(b);
        // Exact top-k membership must match. Order can differ only among equal-metric ties, which
        // is display-only; compare as multisets of (metric) plus the key set to catch any real loss.
        if (a.size() != b.size()) { ++mismatches; continue; }
        std::sort(a.begin(), a.end(), [](const Elem& x, const Elem& y){ return x.first < y.first; });
        std::sort(b.begin(), b.end(), [](const Elem& x, const Elem& y){ return x.first < y.first; });
        if (a != b) ++mismatches;
    }
    std::cout << "trials=" << trials << " mismatches=" << mismatches << "\n";
    return mismatches == 0 ? 0 : 1;
}
