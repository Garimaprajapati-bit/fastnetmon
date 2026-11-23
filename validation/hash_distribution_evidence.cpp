// Evidence for the shard-routing fix: shows the EXACT stored IPv4 key values for addresses in the
// affected prefix and the shard histograms under (a) identity hash %N (the original) vs (b) the
// mix_bits avalanche %N (the fix), using the same key construction the daemon's allocator uses.
//   g++ -O2 -std=c++20 hash_distribution_evidence.cpp -o /tmp/hde && /tmp/hde
#include <cstdio>
#include <cstdint>
#include <arpa/inet.h>
#include <vector>
#include <functional>
#include <cmath>

// mix_bits: identical to abstract_subnet_counters.hpp
static inline uint64_t mix_bits(uint64_t h){ h^=h>>33; h*=0xff51afd7ed558ccdULL; h^=h>>33;
                                             h*=0xc4ceb9fe1a85ec53ULL; h^=h>>33; return h; }

int main(){
    const size_t N = 64;                 // shards (as at 4 workers: threads*16)
    const uint64_t HOSTS = 10485760;     // 10.0.0.0/9 + 10.128.0.0/11 worth, as the daemon allocates

    // How the daemon stores the key (fastnetmon.cpp subnet_vectors_allocator):
    //   host-order ip = ntoh(subnet) + i ; stored key = hton(host-order ip)  (network byte order)
    printf("Stored-key construction (network byte order), first 6 hosts of 10.0.0.0/8:\n");
    for (uint32_t i = 0; i < 6; i++) {
        uint32_t host_order = 0x0A000000u | i;          // 10.0.0.i
        uint32_t stored     = htonl(host_order);        // what preallocate_key(key) receives
        struct in_addr a; a.s_addr = stored;
        printf("  %-10s  host_order=0x%08X  stored_key(uint32)=0x%08X=%u   stored%%%zu=%llu   mix%%%zu=%llu\n",
               inet_ntoa(a), host_order, stored, stored, N,
               (unsigned long long)(std::hash<uint32_t>{}(stored) % N), N,
               (unsigned long long)(mix_bits(std::hash<uint32_t>{}(stored)) % N));
    }

    std::vector<uint64_t> hist_id(N, 0), hist_mix(N, 0);
    for (uint64_t i = 0; i < HOSTS; i++) {
        uint32_t host_order = 0x0A000000u | (uint32_t)(i & 0x00FFFFFFu); // spread across 10.0.0.0/8
        uint32_t stored     = htonl(host_order);
        uint64_t h          = std::hash<uint32_t>{}(stored);            // identity on libstdc++
        hist_id [ h          % N ]++;
        hist_mix[ mix_bits(h) % N ]++;
    }
    auto summarize = [&](const char* name, std::vector<uint64_t>& h){
        uint64_t mn=~0ull, mx=0, nonempty=0, tot=0; double mean=(double)HOSTS/N, var=0;
        for (auto c : h){ if(c<mn)mn=c; if(c>mx)mx=c; if(c)nonempty++; tot+=c; var += (c-mean)*(c-mean); }
        double sd = std::sqrt(var/N);
        printf("  %-9s shards_used=%llu/%zu  min=%llu  max=%llu  mean=%.0f  cv=%.4f  max/mean=%.1f\n",
               name, (unsigned long long)nonempty, N, (unsigned long long)mn, (unsigned long long)mx,
               mean, (mean>0?sd/mean:0), (mean>0?mx/mean:0));
    };
    printf("\nShard histogram over %llu hosts across %zu shards:\n", (unsigned long long)HOSTS, N);
    summarize("identity", hist_id);
    summarize("mix_bits", hist_mix);
    printf("\nInterpretation: identity hash on network-order 10.x keys concentrates hosts into very few\n"
           "shards (one worker does nearly all the work); mix_bits spreads them evenly (cv<1%%, no empties).\n");
    return 0;
}
