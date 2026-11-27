// Callback-exception regression test for the parallel sweep.
// A detection callback that THROWS inside worker threads must be captured and rethrown exactly once
// after all workers join: no crash, no hang, no lost threads, and the container stays usable afterwards
// (a subsequent clean sweep and all lookups still work). Uses the REAL container at the build commit.
//   g++ -O2 -std=c++20 -I src -I src/tests validation/callback_exception_test.cpp -o /tmp/cbx -lpthread -llog4cpp && /tmp/cbx
//   TSan: add -fsanitize=thread   (exercises the exception_ptr/error_mutex path across workers)
#include "abstract_subnet_counters.hpp"
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <atomic>

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
    const uint32_t K=200000; const double period=1.0, avg_time=10.0;
    int fails=0;

    // CASE A: speed_check_callback throws on ONE key -> exactly one exception rethrown, no crash/hang.
    {
        Ctr c; c.configure_sharding(64,8);
        for(uint32_t k=1;k<=K;k++){ auto fn=[&](subnet_counter_t& x){ setfill(x,k); }; c.modify_counter_for_key(k,fn); }
        std::atomic<uint64_t> calls{0};
        int caught=0;
        try{
            c.recalculate_speed(period, avg_time,
                [&](const uint32_t& key, const subnet_counter_t&){
                    calls.fetch_add(1,std::memory_order_relaxed);
                    if(key==123457u) throw std::runtime_error("boom-one");
                }, nullptr);
        }catch(const std::exception&){ caught++; }
        // regaining control proves all workers joined (no hang/leak). Container must stay usable:
        c.recalculate_speed(period, avg_time);          // clean sweep must not crash
        subnet_counter_t a{}; bool stillok = c.get_average_speed(1,a) && c.average_speed_size()==K;
        bool pass = (caught==1) && (calls.load()>0) && stillok;
        printf("CASE A (throw on 1 key): rethrew=%d calls=%llu recovered=%d size=%zu => %s\n",
               caught,(unsigned long long)calls.load(),(int)stillok,(size_t)c.average_speed_size(), pass?"PASS":"FAIL");
        if(!pass) fails++;
    }

    // CASE B: new_speed_calc_callback throws on MANY keys across shards -> still exactly one propagated.
    {
        Ctr c; c.configure_sharding(64,8);
        for(uint32_t k=1;k<=K;k++){ auto fn=[&](subnet_counter_t& x){ setfill(x,k); }; c.modify_counter_for_key(k,fn); }
        int caught=0;
        try{
            c.recalculate_speed(period, avg_time, nullptr,
                [&](const uint32_t& key, subnet_counter_t&, double){
                    if((key%1000u)==0u) throw std::runtime_error("boom-many");
                });
        }catch(...){ caught++; }
        c.recalculate_speed(period, avg_time);          // recover
        bool pass = (caught==1) && (c.average_speed_size()==K);
        printf("CASE B (throw on many keys/shards): exceptions_propagated=%d recovered_size=%zu => %s\n",
               caught,(size_t)c.average_speed_size(), pass?"PASS":"FAIL");
        if(!pass) fails++;
    }

    printf("callback-exception test: %s\n", fails==0?"ALL PASS":"FAIL");
    return fails==0?0:1;
}
