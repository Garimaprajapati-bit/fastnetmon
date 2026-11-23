// Directly exercises the REAL get_top_k_average_speed() in abstract_subnet_counters.hpp
// (the dense + lazy-view implementation) and compares its result to a brute-force full sort,
// over many randomized multi-shard layouts. Catches any error in the transform-view top-k refactor.
// Build: g++ -O2 -std=c++20 -I src -I src/tests topk_header_test.cpp -o /tmp/tkh -lpthread -llog4cpp
#include "abstract_subnet_counters.hpp"
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>

// Verbatim EWMA helpers (as in the other tests) so recalculate_speed() can fill `average`.
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

int main(){
    std::mt19937 rng(12345);
    int trials = 300, mismatches = 0;
    const auto sorter = attack_detection_threshold_type_t::packets_per_second;
    const auto dir    = attack_detection_direction_type_t::incoming;
    const TrafficComparatorClass<std::pair<uint32_t, subnet_counter_t>> cmp(dir, sorter);

    for (int t = 0; t < trials; t++) {
        uint32_t N = 1 + rng() % 4000;           // hosts
        size_t   k = 1 + rng() % 50;             // rows wanted
        size_t shards = 1 + rng() % 16, threads = 1 + rng() % 4;
        Ctr c; c.configure_sharding(shards, threads);
        std::vector<std::pair<uint32_t, subnet_counter_t>> all;
        for (uint32_t i = 1; i <= N; i++) {
            uint32_t ip = rng();
            c.modify_counter_for_key(ip, [&](subnet_counter_t& x){
                x.total.in_packets = rng() % 100000;   // varied + ties
                x.total.in_bytes   = x.total.in_packets * 64;
            });
        }
        c.recalculate_speed(1.0, 5.0);               // fill `average`
        // brute-force reference: gather all averages, full-sort, take k
        std::vector<std::pair<uint32_t, subnet_counter_t>> ref;
        { std::unordered_map<uint32_t, subnet_counter_t> m; c.get_all_average_speed_elements(m);
          ref.assign(m.begin(), m.end()); }
        std::sort(ref.begin(), ref.end(), cmp);
        if (ref.size() > k) ref.resize(k);

        std::vector<std::pair<uint32_t, subnet_counter_t>> got(std::min(k, (size_t)N));
        c.get_top_k_average_speed(got, sorter, dir);

        // compare the metric sequence (ties on metric may reorder keys, so compare the sorted metric
        // multiset of the top-k, which is what "top-k by metric" guarantees)
        auto metric = [](const std::pair<uint32_t, subnet_counter_t>& e){ return e.second.total.in_packets; };
        std::vector<uint64_t> a, b;
        for (auto& e : ref) a.push_back(metric(e));
        for (auto& e : got) b.push_back(metric(e));
        a.resize(std::min(a.size(), b.size())); b.resize(a.size());
        if (a != b) { mismatches++; if (mismatches<=3) printf("  mismatch trial=%d N=%u k=%zu shards=%zu\n", t, N, k, shards); }
    }
    printf("get_top_k header test: trials=%d mismatches=%d => %s\n", trials, mismatches, mismatches==0?"PASS":"FAIL");
    return mismatches==0?0:1;
}
