// Removal / purge regression test for the dense sharded container.
// Exercises purge_old_data() (swap-with-last + index fix) and proves:
//   (1) exactly the stale keys are removed, (2) every survivor is STILL retrievable with its exact
//   pre-purge average (so the index stayed consistent across swap-with-last moves), (3) purged keys
//   are gone, (4) size is correct, (5) re-insert after purge works. Uses the REAL production
//   container at the current build commit (not a stand-in).
//   g++ -O2 -std=c++20 -I src -I src/tests validation/removal_test.cpp -o /tmp/rm -lpthread -llog4cpp && /tmp/rm
#include "abstract_subnet_counters.hpp"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <ctime>

// verbatim EWMA free-functions (as in equiv_concurrency_test.cpp)
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
    const uint32_t K = 20000;
    const double period=1.0, avg_time=10.0;
    Ctr c; c.configure_sharding(16, 1);
    for(uint32_t k=1;k<=K;k++){ auto fn=[&](subnet_counter_t& x){ setfill(x,k); }; c.modify_counter_for_key(k, fn); }

    // give every key a distinctive non-zero average
    c.recalculate_speed(period, avg_time);

    // snapshot each key's average BEFORE purge
    std::unordered_map<uint32_t, subnet_counter_t> snap; snap.reserve(K*2);
    int missing_before=0;
    for(uint32_t k=1;k<=K;k++){ subnet_counter_t a{}; if(!c.get_average_speed(k,a)) missing_before++; snap[k]=a; }

    // mark odd keys recent (keep), even keys stale (purge) via counters[].last_update_time
    time_t now=0; time(&now);
    const int THRESH = 100000;
    uint32_t expect_survivors=0;
    for(uint32_t k=1;k<=K;k++){
        time_t ts = (k & 1u) ? now : (time_t)1;
        if(k & 1u) expect_survivors++;
        c.modify_counter_for_key(k, [&](subnet_counter_t& x){ x.last_update_time = ts; });
    }

    uint64_t removed = c.purge_old_data(THRESH);
    size_t sz = c.average_speed_size();

    int survivor_ok=0, survivor_bad=0, ghost=0;
    for(uint32_t k=1;k<=K;k++){
        subnet_counter_t a{}; bool present=c.get_average_speed(k,a);
        if(k & 1u){
            if(present && std::memcmp(&a,&snap[k],sizeof(subnet_counter_t))==0) survivor_ok++;
            else survivor_bad++;
        } else {
            if(present) ghost++;
        }
    }
    // re-insert a previously-purged key must work
    c.modify_counter_for_key(2, [&](subnet_counter_t& x){ setfill(x,2); x.last_update_time=now; });
    subnet_counter_t re{}; bool reok=c.get_average_speed(2,re);

    bool pass = (missing_before==0) && (removed==(uint64_t)(K-expect_survivors)) && (sz==expect_survivors)
             && (survivor_ok==(int)expect_survivors) && (survivor_bad==0) && (ghost==0) && reok;
    printf("REMOVAL purge_old_data: K=%u removed=%llu (expect %u) size=%zu (expect %u)\n",
           K,(unsigned long long)removed,K-expect_survivors,sz,expect_survivors);
    printf("  survivors_retrievable_exact=%d/%u  corrupted=%d  ghosts=%d  reinsert_ok=%d  => %s\n",
           survivor_ok,expect_survivors,survivor_bad,ghost,(int)reok, pass?"PASS":"FAIL");
    return pass?0:1;
}
