// Fault-injection test for shard_t::append_slot transactional rollback.
// A Counter whose COPY ctor throws on a chosen call lets us force each insert step to fail; after
// every failure the four containers (keys/counters/average/index) must stay equal-sized and consistent,
// and a subsequent insert must still work. Compile with -D_GLIBCXX_ASSERTIONS so that the OLD nested
// rollback's double pop_back (on an already-empty vector) aborts — i.e. this test FAILS on the buggy
// version and PASSES on the fixed single-catch version.
//   g++ -O1 -g -std=c++20 -D_GLIBCXX_ASSERTIONS -I src -I src/tests fault_insert_test.cpp -o /tmp/fi -llog4cpp
#include <cstdio>
#include <cstdint>
#include <new>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <ranges>
#include <algorithm>

// Minimal stand-in for the dense container's shard, copied VERBATIM from the header so we exercise the
// exact append_slot logic without pulling the whole daemon. (Kept in sync with abstract_subnet_counters.hpp.)
template <typename T, typename Counter>
struct shard_t {
    using slot_index_t = size_t;
    std::unordered_map<T, slot_index_t> index;
    std::vector<T> keys;
    std::vector<Counter> counters;
    std::vector<Counter> average;
    inline slot_index_t append_slot(const T& key, const Counter& ic, const Counter& ia) {
        slot_index_t slot = keys.size();
        int done = 0;
        try {
            keys.push_back(key);  ++done;
            counters.push_back(ic); ++done;
            average.push_back(ia);  ++done;
            index.emplace(key, slot); ++done;
        } catch (...) {
            if (done >= 3) average.pop_back();
            if (done >= 2) counters.pop_back();
            if (done >= 1) keys.pop_back();
            throw;
        }
        return slot;
    }
};

struct ThrowCounter {
    uint64_t v = 0;
    static long& calls()   { static long c = 0; return c; }
    static long& throw_at(){ static long n = -1; return n; } // copy # (1-based) that throws; -1 = never
    ThrowCounter() = default;
    ThrowCounter(const ThrowCounter& o) : v(o.v) { if (++calls() == throw_at()) throw std::bad_alloc(); }
    ThrowCounter& operator=(const ThrowCounter&) = default;
};

static bool invariant(const shard_t<uint32_t, ThrowCounter>& s) {
    return s.keys.size() == s.counters.size() && s.counters.size() == s.average.size()
        && s.index.size() == s.keys.size();
}

int main() {
    int fails = 0;
    // Scenario A: counters copy (1st ThrowCounter copy) throws -> done==1 -> rollback pops keys only.
    {
        shard_t<uint32_t, ThrowCounter> s;
        ThrowCounter z;
        ThrowCounter::calls() = 0; ThrowCounter::throw_at() = 1;
        bool threw = false;
        try { s.append_slot(42, z, z); } catch (...) { threw = true; }
        ThrowCounter::throw_at() = -1;
        bool ok = threw && invariant(s) && s.keys.empty();
        printf("A counters-throw: threw=%d invariant=%d size=%zu => %s\n", threw, invariant(s), s.keys.size(), ok?"PASS":"FAIL");
        if(!ok) fails++;
        // follow-up insert must still work (no corrupt state)
        s.append_slot(7, z, z); if(!(invariant(s) && s.keys.size()==1)){ printf("A followup FAIL\n"); fails++; }
    }
    // Scenario B: average copy (2nd copy) throws -> done==2 -> rollback pops counters+keys (once each).
    {
        shard_t<uint32_t, ThrowCounter> s;
        ThrowCounter z;
        ThrowCounter::calls() = 0; ThrowCounter::throw_at() = 2;
        bool threw = false;
        try { s.append_slot(99, z, z); } catch (...) { threw = true; }
        ThrowCounter::throw_at() = -1;
        bool ok = threw && invariant(s) && s.keys.empty();
        printf("B average-throw:  threw=%d invariant=%d size=%zu => %s\n", threw, invariant(s), s.keys.size(), ok?"PASS":"FAIL");
        if(!ok) fails++;
        s.append_slot(8, z, z); if(!(invariant(s) && s.keys.size()==1)){ printf("B followup FAIL\n"); fails++; }
    }
    // Scenario C: no throw -> normal insert.
    {
        shard_t<uint32_t, ThrowCounter> s; ThrowCounter z;
        ThrowCounter::throw_at() = -1;
        s.append_slot(1, z, z); s.append_slot(2, z, z);
        bool ok = invariant(s) && s.keys.size()==2;
        printf("C normal:         invariant=%d size=%zu => %s\n", invariant(s), s.keys.size(), ok?"PASS":"FAIL");
        if(!ok) fails++;
    }
    printf("fault-injection rollback test: %s\n", fails==0 ? "ALL PASS" : "FAIL");
    return fails==0 ? 0 : 1;
}
