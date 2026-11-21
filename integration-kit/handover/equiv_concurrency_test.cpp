// Behaviour-preservation + concurrency test for the sharded parallel speed sweep.
// Uses the REAL production container (src/abstract_subnet_counters.hpp) and the REAL
// subnet_counter_t; the two EWMA free-functions are verbatim copies of src/speed_counters.cpp
// (proven byte-identical) so we don't have to link the whole daemon.
//
// Build (from repo root):
//   g++ -O2 -std=c++20 -I src -I src/tests fastnetmon-audit/tests/equiv_concurrency_test.cpp -o /tmp/eqc -lpthread
//   TSan:  add -fsanitize=thread   | ASan+UBSan: add -fsanitize=address,undefined
// Run:  /tmp/eqc
//
// TEST 1 (equivalence): a sequential container (1 shard, 1 thread) and a parallel container
//   (64 shards, 8 threads) fed identical traffic must produce byte-identical averages.
// TEST 2 (concurrency): parallel recalc sweeps run WHILE other threads do per-key increments
//   and reads; under ThreadSanitizer this must report zero data races.
#include "abstract_subnet_counters.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>
#include <random>

// --- verbatim from src/speed_counters.cpp (exact integer-truncation EWMA) ---
void build_speed_counters_from_packet_counters(subnet_counter_t& n, const subnet_counter_t& d, double p){
    n.total.calculate_speed(d.total,p); n.dropped.calculate_speed(d.dropped,p); n.fragmented.calculate_speed(d.fragmented,p);
    n.tcp_syn.calculate_speed(d.tcp_syn,p); n.tcp.calculate_speed(d.tcp,p); n.udp.calculate_speed(d.udp,p); n.icmp.calculate_speed(d.icmp,p);
}
void build_average_speed_counters_from_speed_counters(subnet_counter_t& a, const subnet_counter_t& n, double e){
    a.total.calulate_exponential_moving_average_speed(n.total,e); a.dropped.calulate_exponential_moving_average_speed(n.dropped,e);
    a.fragmented.calulate_exponential_moving_average_speed(n.fragmented,e); a.tcp_syn.calulate_exponential_moving_average_speed(n.tcp_syn,e);
    a.tcp.calulate_exponential_moving_average_speed(n.tcp,e); a.udp.calulate_exponential_moving_average_speed(n.udp,e); a.icmp.calulate_exponential_moving_average_speed(n.icmp,e);
    a.out_flows=uint64_t(n.out_flows+e*((double)a.out_flows-(double)n.out_flows)); a.in_flows=uint64_t(n.in_flows+e*((double)a.in_flows-(double)n.in_flows));
}

using Ctr = abstract_subnet_counters_t<uint32_t, subnet_counter_t>;
static void setfill(subnet_counter_t& c, uint32_t key){
    c.total.in_packets=1000+(key&0xFFF); c.total.out_packets=500+(key&0x7FF);
    c.total.in_bytes=c.total.in_packets*800; c.total.out_bytes=c.total.out_packets*800;
    c.tcp=c.total; c.udp=c.total; c.icmp=c.total; c.in_flows=key&0x3F; c.out_flows=key&0x1F;
}

int main(){
    const uint32_t K = 200000;          // keys
    const double period = 1.0, avg_time = 10.0;

    // ---------------- TEST 1: equivalence (sequential vs parallel) ----------------
    Ctr seqc, parc;
    seqc.configure_sharding(1, 1);      // 1 shard,  sequential  (== original behaviour)
    parc.configure_sharding(64, 8);     // 64 shards, 8 workers   (parallel)
    for (uint32_t k = 1; k <= K; k++) {
        auto fn = [&](subnet_counter_t& c){ setfill(c, k); };
        seqc.modify_counter_for_key(k, fn);
        parc.modify_counter_for_key(k, fn);
    }
    seqc.recalculate_speed(period, avg_time);   // no callbacks -> pure sweep
    parc.recalculate_speed(period, avg_time);
    int mism = 0;
    for (uint32_t k = 1; k <= K; k++) {
        subnet_counter_t a{}, b{};
        bool ra = seqc.get_average_speed(k, a), rb = parc.get_average_speed(k, b);
        if (!ra || !rb || std::memcmp(&a, &b, sizeof(subnet_counter_t)) != 0) mism++;
    }
    std::printf("TEST1 equivalence (seq 1-shard vs parallel 64-shard/8-thread): K=%u byte-mismatches=%d  => %s\n",
                K, mism, mism==0 ? "PASS" : "FAIL");

    // ---------------- TEST 2: concurrency (TSan target) ----------------
    Ctr c; c.configure_sharding(64, 8);
    for (uint32_t k = 1; k <= K; k++){ auto fn=[&](subnet_counter_t& x){ setfill(x,k); }; c.modify_counter_for_key(k, fn); }
    std::atomic<bool> stop{false}; std::atomic<uint64_t> cb{0};
    std::vector<std::thread> bg;
    for (int w=0; w<4; w++) bg.emplace_back([&,w]{ std::mt19937 r(100+w);
        while(!stop.load(std::memory_order_relaxed)){ uint32_t k=1+(r()%K);
            c.modify_counter_for_key(k, [](subnet_counter_t& x){ x.total.in_packets+=1; x.total.in_bytes+=64; }); } });
    for (int rdr=0; rdr<2; rdr++) bg.emplace_back([&,rdr]{ std::mt19937 r(7+rdr);
        while(!stop.load(std::memory_order_relaxed)){ subnet_counter_t a{}; c.get_average_speed(1+(r()%K), a); } });
    // main thread: repeated parallel sweeps with a callback (mirrors detection callback being invoked by workers)
    for (int it=0; it<30; it++)
        c.recalculate_speed(period, avg_time,
            [&](const uint32_t&, const subnet_counter_t&){ cb.fetch_add(1, std::memory_order_relaxed); }, nullptr);
    stop.store(true); for(auto& t: bg) t.join();
    std::printf("TEST2 concurrency: 30 parallel sweeps + 4 writers + 2 readers, callback_invocations=%llu  => PASS if sanitizer-clean\n",
                (unsigned long long)cb.load());
    return mism==0 ? 0 : 1;
}
