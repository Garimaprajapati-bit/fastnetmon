// Scaling benchmark for the sharded per-host speed calculation.
//
// Populates a fixed number of hosts, then times the REAL recalculate_speed() sweep at
// increasing worker-thread counts to show parallel scaling. Links the production
// counter code (abstract_subnet_counters.hpp + speed_counters.cpp); the sweep itself is
// exactly what the daemon runs once per second.
//
// Build (from src/tests):
//   g++ -std=c++17 -O2 -I.. speed_calculation_scaling_test.cpp ../speed_counters.cpp \
//       -llog4cpp -pthread -o /tmp/scaling
// Run:
//   /tmp/scaling <hosts> <shards>      e.g.  /tmp/scaling 10000000 256

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "../abstract_subnet_counters.hpp"

// The two symbols the counter code references, provided here for the standalone harness.
time_t current_inaccurate_time = 1000;
int extract_bit_value(uint8_t num, int bit) { return (num & (1 << bit)) ? 1 : 0; }

using Counters = abstract_subnet_counters_t<uint32_t, subnet_counter_t>;

static double time_sweep(Counters& c, size_t threads) {
    c.recalc_threads = threads;
    c.recalculate_speed(1.0, 15.0, nullptr, nullptr); // warmup
    const int ITERS = 10;
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < ITERS; k++) c.recalculate_speed(1.0, 15.0, nullptr, nullptr);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / ITERS;
}

int main(int argc, char** argv) {
    long   hosts      = argc > 1 ? atol(argv[1]) : 10000000;
    size_t shards     = argc > 2 ? (size_t)atol(argv[2]) : 256;
    unsigned hw       = std::thread::hardware_concurrency();
    size_t maxthreads = argc > 3 ? (size_t)atol(argv[3]) : (hw ? hw : 8);

    printf("hosts=%ld shards=%zu detected_cores=%u\n", hosts, shards, hw);
    fflush(stdout);

    Counters c(shards);
    simple_packet_t p; p.protocol = 6; p.length = 100;
    for (long i = 0; i < hosts; i++) c.increment_incoming_counters_for_key((uint32_t)i, p, 10, 1000);
    printf("populated %zu host counters (%.1f GB resident approx)\n\n", c.size(),
           (double)c.size() * (sizeof(subnet_counter_t) * 2 + 64) / 1e9);

    printf("%-10s %12s %14s %10s\n", "threads", "sweep(ms)", "M hosts/s", "speedup");
    double base = 0;
    for (size_t th = 1; th <= maxthreads; th *= 2) {
        double ms = time_sweep(c, th);
        if (th == 1) base = ms;
        printf("%-10zu %12.1f %14.1f %9.2fx\n", th, ms, hosts / 1e6 / (ms / 1000.0), base / ms);
        fflush(stdout);
    }
    // Non-power-of-two top core count (e.g. 6, 12), if applicable.
    if ((maxthreads & (maxthreads - 1)) != 0) {
        double ms = time_sweep(c, maxthreads);
        printf("%-10zu %12.1f %14.1f %9.2fx\n", maxthreads, ms, hosts / 1e6 / (ms / 1000.0), base / ms);
    }
    return 0;
}
