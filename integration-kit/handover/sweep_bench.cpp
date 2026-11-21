// Step-1 isolated container benchmark (audit-area only; NOT production code).  v2 (methodology fixes)
//
// Fixes after review:
//  (1) shard count was coupled to thread count; now runs BOTH production-auto (threads*16) AND a
//      fixed shard count, so "structure change" is separated from "worker change".
//  (2) all candidates use the SAME partition COUNT; added unordered_range so (hash-vs-range) isolates
//      work-distribution and (range-vs-dense) isolates pure memory layout. dense is labelled range.
//  (3) memory column renamed to estimated container storage; a real peak-RSS (VmHWM) line is printed
//      for a fresh-process-per-candidate driver (run_bench.sh) — hardcoded bytes are estimates only.
//  (4) full-field FNV checksum over EVERY EWMA output field (7 elements x4 + both flows), not 3 fields.
//
// Real subnet_counter_t (248 B, static_assert'd) + verbatim EWMA (exact integer truncation).
// Build (from repo root) — NOTE: does NOT link speed_counters.cpp (EWMA is copied in); needs absl libs:
//   g++ -O2 -std=c++20 -I src -I src/tests fastnetmon-audit/bench/sweep_bench.cpp \
//       $(pkg-config --cflags --libs absl_flat_hash_map) -o /tmp/sweep_bench -lpthread
// Run:  /tmp/sweep_bench <N> <threads_csv> <repeats> <shardmode: auto|NUM> [only_candidate]
#include "subnet_counter.hpp"   // self-contained; element EWMA methods are inline here
#include <absl/container/flat_hash_map.h>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <string>
#include <fstream>
#include <malloc.h>
#include <cstring>

static_assert(sizeof(subnet_counter_t) == 248, "struct size drifted from measured production size");
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b){ return std::chrono::duration<double,std::milli>(b-a).count(); }
static long vmhwm_kb(){ std::ifstream f("/proc/self/status"); std::string k;
    while (f>>k){ if(k=="VmHWM:"){ long v; f>>v; return v; } } return 0; }
static double med(std::vector<double> v){ std::sort(v.begin(),v.end()); return v.empty()?0:v[v.size()/2]; }
static double lo(std::vector<double> v){ std::sort(v.begin(),v.end()); return v.empty()?0:v.front(); }
static double hi(std::vector<double> v){ std::sort(v.begin(),v.end()); return v.empty()?0:v.back(); }
static inline uint32_t H(uint32_t k){ return k*2654435761u; }

static void fill(subnet_counter_t& c, uint32_t key){
    c.total.in_packets=1000+(key&0xFFF); c.total.out_packets=500+(key&0x7FF);
    c.total.in_bytes=c.total.in_packets*800; c.total.out_bytes=c.total.out_packets*800;
    c.tcp=c.total; c.udp=c.total; c.icmp=c.total; c.last_update_time=key;
}
// verbatim copies of src/speed_counters.cpp (exact truncation); element methods are the real inline ones
static void build_speed(subnet_counter_t& n, const subnet_counter_t& d, double p){
    n.total.calculate_speed(d.total,p); n.dropped.calculate_speed(d.dropped,p); n.fragmented.calculate_speed(d.fragmented,p);
    n.tcp_syn.calculate_speed(d.tcp_syn,p); n.tcp.calculate_speed(d.tcp,p); n.udp.calculate_speed(d.udp,p); n.icmp.calculate_speed(d.icmp,p); }
static void build_avg(subnet_counter_t& a, const subnet_counter_t& n, double e){
    a.total.calulate_exponential_moving_average_speed(n.total,e); a.dropped.calulate_exponential_moving_average_speed(n.dropped,e);
    a.fragmented.calulate_exponential_moving_average_speed(n.fragmented,e); a.tcp_syn.calulate_exponential_moving_average_speed(n.tcp_syn,e);
    a.tcp.calulate_exponential_moving_average_speed(n.tcp,e); a.udp.calulate_exponential_moving_average_speed(n.udp,e); a.icmp.calulate_exponential_moving_average_speed(n.icmp,e);
    a.out_flows=uint64_t(n.out_flows+e*((double)a.out_flows-(double)n.out_flows)); a.in_flows=uint64_t(n.in_flows+e*((double)a.in_flows-(double)n.in_flows)); }
static inline void sweep_one(const subnet_counter_t& cnt, subnet_counter_t& avg, double ev){
    subnet_counter_t ns; build_speed(ns,cnt,1.0); build_avg(avg,ns,ev); }
// INDEPENDENT oracle: recompute the expected average for a key from scratch (R sweeps of its fixed
// counter) and byte-compare against the candidate's stored average. Catches a shared bug the
// cross-candidate checksum could not, and is exact (no hash-collision risk). Returns 1 on mismatch.
static int oracle_mismatch(uint32_t key, const subnet_counter_t& got, unsigned R, double ev){
    subnet_counter_t cnt{}; fill(cnt,key); subnet_counter_t exp{};
    for(unsigned r=0;r<R;r++) sweep_one(cnt,exp,ev);
    return std::memcmp(&exp,&got,sizeof(subnet_counter_t))!=0; }
// full-field FNV-1a over every EWMA output field (catches any divergence, not just 3 fields)
static inline void fmix(uint64_t& h,uint64_t v){ h^=v; h*=1099511628211ull; }
static inline void felem(uint64_t& h,const traffic_counter_element_t& t){ fmix(h,t.in_packets);fmix(h,t.out_packets);fmix(h,t.in_bytes);fmix(h,t.out_bytes); }
static inline void facc(uint64_t& h,const subnet_counter_t& a){ felem(h,a.total);felem(h,a.dropped);felem(h,a.fragmented);felem(h,a.tcp_syn);felem(h,a.tcp);felem(h,a.udp);felem(h,a.icmp);fmix(h,a.in_flows);fmix(h,a.out_flows); }
// per-key field hash; summed across keys => COMMUTATIVE (order-independent) full-field equivalence check
static inline uint64_t keyh(uint32_t key,const subnet_counter_t& a){ uint64_t h=1469598103934665603ull; fmix(h,key); facc(h,a); return h; }

struct Block{ uint32_t base,count; };
static std::vector<Block> blocks(size_t t){ if(t<=(1u<<20)) return {{0x0A000000u,1u<<20}}; return {{0x0A000000u,1u<<23},{0x0A800000u,1u<<21}}; }
static std::vector<uint32_t> make_keys(const std::vector<Block>& bl){ std::vector<uint32_t> k; for(auto&b:bl) for(uint32_t i=0;i<b.count;i++) k.push_back(b.base+i); return k; }

template<class Fn> static double par(unsigned nparts, unsigned threads, Fn fn){
    std::atomic<unsigned> next{0}; auto body=[&]{ unsigned p; while((p=next.fetch_add(1))<nparts) fn(p); };
    auto t0=Clock::now();
    if(threads<=1){ for(unsigned p=0;p<nparts;p++) fn(p); }
    else { std::vector<std::thread> w; for(unsigned t=0;t<threads;t++) w.emplace_back(body); for(auto&x:w) x.join(); }
    return ms(t0,Clock::now());
}
struct OpenAddr { std::vector<uint32_t> key; std::vector<subnet_counter_t> val; std::vector<uint8_t> used; size_t mask=0;
    void init(size_t n){ size_t c=16; while(c<n*10/7) c<<=1; key.assign(c,0); val.resize(c); used.assign(c,0); mask=c-1; }
    subnet_counter_t& ins(uint32_t k){ size_t i=H(k)&mask; while(used[i]){ if(key[i]==k) return val[i]; i=(i+1)&mask; } used[i]=1; key[i]=k; return val[i]; }
    subnet_counter_t* find(uint32_t k){ size_t i=H(k)&mask; while(used[i]){ if(key[i]==k) return &val[i]; i=(i+1)&mask; } return nullptr; } };

int main(int argc,char**argv){
    size_t Nreq=argc>1?std::stoul(argv[1]):1000000;
    std::vector<unsigned> THS; { std::string s=argc>2?argv[2]:"1,2,4"; size_t p=0,c; while((c=s.find(',',p))!=std::string::npos){ THS.push_back(std::stoul(s.substr(p,c-p))); p=c+1;} THS.push_back(std::stoul(s.substr(p))); }
    unsigned R=argc>3?std::stoul(argv[3]):5; std::string smode=argc>4?argv[4]:"auto"; std::string only=argc>5?argv[5]:"";
    const double ev=0.5;
    auto bl=blocks(Nreq); auto keys=make_keys(bl); size_t N=keys.size();
    bool want=[&](const char*n){ return only.empty()||only==n; }("");
    (void)want;
    std::printf("# N=%zu hosts (248 B), repeats=%u, threads=%s, shardmode=%s%s\n", N,R,argc>2?argv[2]:"1,2,4",smode.c_str(), only.empty()?"":(" only="+only).c_str());
    std::printf("# %-15s %4s %6s %9s %9s %9s %9s %9s %10s %9s %s\n","candidate","thr","parts","build_ms","sweep_med","sweep_min","sweep_max","Mhost/s","est_store_MB","lookup_ms","full_checksum");
    std::vector<uint32_t> probe(2000000); { uint64_t r=88172645463325252ull; for(auto&x:probe){ r^=r<<13;r^=r>>7;r^=r<<17; x=keys[r%N]; } }
    uint64_t ref=0; bool haveref=false; auto chk=[&](uint64_t cs,const char*n){ if(!haveref){ref=cs;haveref=true;} else if(cs!=ref) std::printf("#  !! CHECKSUM MISMATCH %s: %016llx != %016llx\n",n,(unsigned long long)cs,(unsigned long long)ref); };
    auto pick=[&](const char*n){ return only.empty()||only==n; };
    auto store=[&](double bpe){ return 2.0*N*bpe/1048576.0; };

    for(unsigned th:THS){
        unsigned P = smode=="auto" ? (th>1?th*16u:1u) : (unsigned)std::stoul(smode);
        auto prow=[&](const char*n,double build,std::vector<double>&sw,double memMB,double lk,uint64_t cs){
            double m=med(sw); std::printf("  %-15s %4u %6u %9.1f %9.1f %9.1f %9.1f %9.2f %10.1f %9.1f %016llx\n",
                n,th,P,build,m,lo(sw),hi(sw),(m>0?N/1e6/(m/1e3):0),memMB,lk,(unsigned long long)cs); };

        // generic map runner; partfn(i,k)->partition in [0,P)
        auto run_map=[&](const char* name,double bpe,auto maker,auto partfn){ if(!pick(name)) return;
            auto b0=Clock::now(); auto cm=maker(P), am=maker(P);
            for(size_t i=0;i<N;i++){ uint32_t k=keys[i]; unsigned s=partfn(i,k); subnet_counter_t z{}; fill(z,k); cm[s][k]=z; am[s][k]={}; }
            double build=ms(b0,Clock::now());
            std::vector<double> sw; for(unsigned r=0;r<R;r++) sw.push_back(par(P,th,[&](unsigned s){ for(auto&kv:cm[s]) sweep_one(kv.second,am[s][kv.first],ev); }));
            uint64_t cs=0; for(auto&m:am) for(auto&kv:m) cs+=keyh(kv.first,kv.second); chk(cs,name);
            size_t mn=~0ull,mx=0; for(auto&m:cm){ if(m.size()<mn)mn=m.size(); if(m.size()>mx)mx=m.size(); }
            unsigned long vm=0; char vb[24]="n/a(>1M)"; if(N<=(1u<<20)+1){ for(auto&m:am) for(auto&kv:m) vm+=oracle_mismatch(kv.first,kv.second,R,ev); std::snprintf(vb,sizeof vb,"%lu",vm);}
            volatile uint64_t sink=0; auto l0=Clock::now(); for(uint32_t k:probe){ auto&m=cm[partfn(0,k)]; auto it=m.find(k); if(it!=m.end()) sink+=it->second.total.in_packets; }
            prow(name,build,sw,store(bpe),ms(l0,Clock::now()),cs); std::printf("#     %-15s balance[min/max part entries]=%zu/%zu  oracle_mismatch=%s\n",name,mn,mx,vb); (void)sink; malloc_trim(0); };
        auto hashp =[&](size_t,uint32_t k){ return H(k)%P; };
        auto rangep=[&](size_t i,uint32_t){ return (unsigned)((uint64_t)i*P/N); };

        run_map("unordered_hash",272.0,[&](unsigned n){ return std::vector<std::unordered_map<uint32_t,subnet_counter_t>>(n); },hashp);
        run_map("unordered_range",272.0,[&](unsigned n){ return std::vector<std::unordered_map<uint32_t,subnet_counter_t>>(n); },rangep);
        run_map("flat_hash_map",293.0,[&](unsigned n){ return std::vector<absl::flat_hash_map<uint32_t,subnet_counter_t>>(n); },hashp);

        if(pick("open_addr")){ auto b0=Clock::now(); std::vector<OpenAddr> cm(P),am(P); std::vector<size_t> cnt(P,0);
            for(uint32_t k:keys) cnt[H(k)%P]++; for(unsigned s=0;s<P;s++){ cm[s].init(cnt[s]?cnt[s]:1); am[s].init(cnt[s]?cnt[s]:1);}
            for(uint32_t k:keys){ unsigned s=H(k)%P; fill(cm[s].ins(k),k); am[s].ins(k);} double build=ms(b0,Clock::now());
            double memMB=0; for(unsigned s=0;s<P;s++) memMB+=(cm[s].mask+1)+(am[s].mask+1); memMB=memMB*253.0/1048576.0;
            std::vector<double> sw; for(unsigned r=0;r<R;r++) sw.push_back(par(P,th,[&](unsigned s){ auto&C=cm[s];auto&A=am[s]; for(size_t i=0;i<=C.mask;i++) if(C.used[i]) sweep_one(C.val[i],*A.find(C.key[i]),ev); }));
            uint64_t cs=0; for(auto&A:am) for(size_t i=0;i<=A.mask;i++) if(A.used[i]) cs+=keyh(A.key[i],A.val[i]); chk(cs,"open_addr");
            size_t mn=~0ull,mx=0; for(unsigned s=0;s<P;s++){ if(cnt[s]<mn)mn=cnt[s]; if(cnt[s]>mx)mx=cnt[s]; }
            unsigned long vm=0; char vb[24]="n/a(>1M)"; if(N<=(1u<<20)+1){ for(auto&A:am) for(size_t i=0;i<=A.mask;i++) if(A.used[i]) vm+=oracle_mismatch(A.key[i],A.val[i],R,ev); std::snprintf(vb,sizeof vb,"%lu",vm);}
            volatile uint64_t sink=0; auto l0=Clock::now(); for(uint32_t k:probe){ auto p=cm[H(k)%P].find(k); if(p) sink+=p->total.in_packets; }
            prow("open_addr",build,sw,memMB,ms(l0,Clock::now()),cs); std::printf("#     %-15s balance[min/max part entries]=%zu/%zu  oracle_mismatch=%s\n","open_addr",mn,mx,vb); (void)sink; malloc_trim(0); }

        if(pick("dense_vec_range")){ auto b0=Clock::now();
            std::vector<std::vector<subnet_counter_t>> cm(bl.size()),am(bl.size());
            for(size_t bi=0;bi<bl.size();bi++){ cm[bi].resize(bl[bi].count); am[bi].resize(bl[bi].count); for(uint32_t i=0;i<bl[bi].count;i++) fill(cm[bi][i],bl[bi].base+i); }
            double build=ms(b0,Clock::now()); double memMB=2.0*N*248.0/1048576.0;
            std::vector<double> sw; for(unsigned r=0;r<R;r++) sw.push_back(par(P,th,[&](unsigned part){ // SAME partition count P as the maps
                size_t loix=(uint64_t)part*N/P, hiix=(uint64_t)(part+1)*N/P, seen=0;
                for(size_t bi=0;bi<bl.size();bi++){ size_t c=bl[bi].count; if(seen+c<=loix||seen>=hiix){ seen+=c; continue; }
                    size_t a=loix>seen?loix-seen:0, b=hiix-seen<c?hiix-seen:c; for(size_t i=a;i<b;i++) sweep_one(cm[bi][i],am[bi][i],ev); seen+=c; } }));
            uint64_t cs=0; for(size_t bi=0;bi<bl.size();bi++) for(size_t i=0;i<am[bi].size();i++) cs+=keyh(bl[bi].base+(uint32_t)i,am[bi][i]); chk(cs,"dense_vec_range");
            size_t mn=N/P, mx=(N+P-1)/P; unsigned long vm=0; char vb[24]="n/a(>1M)";
            if(N<=(1u<<20)+1){ for(size_t bi=0;bi<bl.size();bi++) for(size_t i=0;i<am[bi].size();i++) vm+=oracle_mismatch(bl[bi].base+(uint32_t)i,am[bi][i],R,ev); std::snprintf(vb,sizeof vb,"%lu",vm);}
            volatile uint64_t sink=0; auto l0=Clock::now(); for(uint32_t k:probe){ for(size_t bi=0;bi<bl.size();bi++) if(k>=bl[bi].base&&k<bl[bi].base+bl[bi].count){ sink+=cm[bi][k-bl[bi].base].total.in_packets; break; } }
            prow("dense_vec_range",build,sw,memMB,ms(l0,Clock::now()),cs); std::printf("#     %-15s balance[min/max part entries]=%zu/%zu  oracle_mismatch=%s\n","dense_vec_range",mn,mx,vb); (void)sink; malloc_trim(0); }
    }
    std::printf("# all checksums should equal %016llx (full-field). peak VmRSS this process: %.1f MB\n",(unsigned long long)ref, vmhwm_kb()/1024.0);
    std::printf("# est_store_MB = estimated container storage (hardcoded per-slot bytes; NOT measured RSS). For real peak RSS run one candidate per process (run_bench.sh) and read the VmRSS line.\n");
    std::printf("# dense_vec_range is a RANGE-PARTITIONED architecture (contiguous CIDR arrays), not a drop-in map swap; decompose: (unordered_hash - unordered_range)=partitioning, (unordered_range - dense_vec_range)=pure layout.\n");
    return 0;
}
