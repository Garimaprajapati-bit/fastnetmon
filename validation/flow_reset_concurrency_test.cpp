// Deterministic concurrent writer/reset test for the sharded connection-tracking store.
// Models sharded_flow_counters_t (N maps + N mutexes + shard_of) with a minimal integer payload, and
// runs many writer threads incrementing flow entries WHILE a reset thread clears shards — exactly the
// daemon's packet-writers-vs-per-second-reset interaction.
//
// Defined expected result (the property that must hold): CONSERVATION. Every unit a writer adds is
// "drained" by exactly one reset (a reset reads a shard's total, adds it to a running drained-total,
// then clears that shard). After all writers finish and a final drain runs,
//     drained_total == issued_total
// must hold exactly — i.e. no write is lost, torn, or double-counted across the concurrent resets,
// regardless of the per-shard (non-global) reset boundary. (The only thing the per-shard boundary
// changes is WHICH reset drains a given write, not whether it is counted.)
//
// Build: g++ -O2 -std=c++20 flow_reset_concurrency_test.cpp -o /tmp/frc -lpthread
//   TSan: add -fsanitize=thread  (expect 0 warnings)
#include <cstdio>
#include <cstdint>
#include <array>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <random>

static inline uint64_t mix(uint32_t ip){ uint64_t h=ip; h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33; return h; }

static const size_t NS = 256;
struct sharded_t {
    std::array<std::map<uint32_t, uint64_t>, NS> maps;  // key -> accumulated units (stand-in for flow counts)
    std::array<std::mutex, NS>                   mu;
    static size_t shard_of(uint32_t ip){ return mix(ip) & (NS - 1); }
    void add(uint32_t ip, uint64_t n){ size_t s=shard_of(ip); std::lock_guard<std::mutex> g(mu[s]); maps[s][ip]+=n; }
    // drain one shard: read its total, clear it, return the total removed (atomic w.r.t. that shard)
    uint64_t drain_shard(size_t s){ std::lock_guard<std::mutex> g(mu[s]); uint64_t sum=0;
        for (auto& kv : maps[s]) sum += kv.second; maps[s].clear(); return sum; }
};

int main(){
    sharded_t store;
    const int WRITERS = 6;
    const uint64_t PER_WRITER = 2'000'000;   // increments each writer issues
    std::atomic<uint64_t> issued{0}, drained{0};
    std::atomic<bool> writing{true};

    std::vector<std::thread> ws;
    for (int w = 0; w < WRITERS; w++) ws.emplace_back([&, w]{
        std::mt19937 rng(1000 + w);
        for (uint64_t i = 0; i < PER_WRITER; i++) {
            uint32_t ip = 0x0A000000u | (rng() & 0x00FFFFFFu);  // 10.x.x.x
            store.add(ip, 1);
            issued.fetch_add(1, std::memory_order_relaxed);
        }
    });
    // reset thread: continuously drains shards while writers run (models the 1s reset racing ingest)
    std::thread resetter([&]{
        std::mt19937 rng(7);
        while (writing.load(std::memory_order_relaxed)) {
            for (size_t s = 0; s < NS; s++) drained.fetch_add(store.drain_shard(s), std::memory_order_relaxed);
        }
    });

    for (auto& t : ws) t.join();
    writing.store(false);
    resetter.join();
    // final drain: capture anything written after the resetter's last pass
    for (size_t s = 0; s < NS; s++) drained.fetch_add(store.drain_shard(s), std::memory_order_relaxed);

    uint64_t iss = issued.load(), drn = drained.load();
    bool ok = (iss == drn);
    printf("flow-reset conservation: writers=%d issued=%llu drained=%llu  => %s\n",
           WRITERS, (unsigned long long)iss, (unsigned long long)drn, ok ? "PASS (no write lost/double-counted)" : "FAIL");
    return ok ? 0 : 1;
}
