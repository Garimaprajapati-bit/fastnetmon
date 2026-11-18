#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <boost/archive/binary_iarchive.hpp>

#include "../abstract_subnet_counters.hpp"
#include "../fast_library.hpp"
#include "../fastnetmon_types.hpp"

#include "../fast_endianless.hpp"

#include "log4cpp/Appender.hh"
#include "log4cpp/BasicLayout.hh"
#include "log4cpp/Category.hh"
#include "log4cpp/FileAppender.hh"
#include "log4cpp/Layout.hh"
#include "log4cpp/OstreamAppender.hh"
#include "log4cpp/PatternLayout.hh"
#include "log4cpp/Priority.hh"

log4cpp::Category& logger = log4cpp::Category::getRoot();

abstract_subnet_counters_t<uint32_t, subnet_counter_t> ipv4_host_counters;
time_t current_inaccurate_time = 0;

// Try to copy source map into vector
template <typename T> void try_to_copy_to_vector(T& counter_map) {
    std::cout << "Evaluate time required to make full copy of structure into std::vector" << std::endl;

    std::vector<std::pair<uint32_t, subnet_counter_t>> counter_map_copy;
    counter_map_copy.reserve(counter_map.size());

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    ssize_t current_index = 0;

    // Try making copy of traffic counters and track time
    for (auto& itr : counter_map) {
        counter_map_copy[current_index++] = itr;
    }

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied traffic map in " << used_time << " seconds" << std::endl << std::endl;
    counter_map_copy.clear();
}

// Try to copy source map into vector
template <typename T> void try_to_copy_to_vector_initilised_elements(T& counter_map) {
    std::cout
        << "Evaluate time required to make full copy of structure into std::vector with all pre-initialised elements"
        << std::endl;

    // 3) Constructs the container with count copies of elements with value value.
    std::vector<std::pair<uint32_t, subnet_counter_t>> counter_map_copy(counter_map.size(), std::make_pair(0, subnet_counter_t{}));

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    ssize_t current_index = 0;

    // Try making copy of traffic counters and track time
    for (auto& itr : counter_map) {
        counter_map_copy[current_index++] = itr;
    }

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied traffic map in " << used_time << " seconds" << std::endl << std::endl;
    counter_map_copy.clear();
}

// Try to copy source map into array
// TODO: this test cannot free up memory, you must run it as last
template <typename T> void try_to_copy_to_array(T& counter_map) {
    std::cout << "Evaluate time required to make full copy of structure into std::array" << std::endl;

    // I've manually set size to bit our 14.6m element array
    std::array<std::pair<uint32_t, subnet_counter_t>, 20000000> counter_map_copy;

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    ssize_t current_index = 0;

    for (auto& itr : counter_map) {
        counter_map_copy[current_index++] = itr;
    }

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied traffic map in " << used_time << " seconds" << std::endl << std::endl;
}


// Try to copy source map into std::unordered_map
template <typename T> void try_to_copy_to_std_unordered_map(T& counter_map, bool reserve) {
    std::cout << "Evaluate time required to make full copy of structure into std::unordered_map";

    if (reserve) {
        std::cout << " with memory reserve in map";
    }

    std::cout << std::endl;

    std::unordered_map<uint32_t, subnet_counter_t> counter_map_copy;

    if (reserve) {
        counter_map_copy.reserve(counter_map.size());
    }

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    // Try making copy of traffic counters and track time
    for (const auto& itr : counter_map) {
        counter_map_copy[itr.first] = itr.second;
    }

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied traffic map in " << used_time << " seconds" << std::endl << std::endl;
    counter_map_copy.clear();
}

// Try to copy source map into std::unordered_map
template <typename T> void try_to_copy_to_std_unordered_map_pre_created_keys(T& counter_map) {
    std::cout << "Evaluate time required to make full copy of structure into std::unordered_map with pre-created keys "
                 "as in source structure";

    std::cout << std::endl;

    std::unordered_map<uint32_t, subnet_counter_t> counter_map_copy;

    // Create keys as in original structure
    for (const auto& itr : counter_map) {
        counter_map_copy[itr.first] = subnet_counter_t{};
    }

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    // Try making copy of traffic counters and track time
    for (const auto& itr : counter_map) {
        counter_map_copy[itr.first] = itr.second;
    }

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied traffic map in " << used_time << " seconds" << std::endl << std::endl;
    counter_map_copy.clear();
}

// Copies memory from std::vector to another std::vector using memory copy
template <typename T> void try_to_copy_vector_to_vector_memory_copy(T& counter_map_copy, const T& counter_map) {
    std::cout
        << "Evaluate time required to make full copy of std::vector into another std::vector using memory region copy"
        << std::endl;

    // We need to allocate as much memory as source std::vector to have enough memory to do copy
    counter_map_copy.reserve(counter_map.size());

    timeval start_time_val;
    gettimeofday(&start_time_val, NULL);

    std::size_t memory_region_size = counter_map.size() * sizeof(std::pair<uint32_t, subnet_counter_t>);

    // Do memory copy
    memcpy((void*)counter_map_copy.data(), (void*)counter_map.data(), memory_region_size);

    timeval finish_time_val;
    gettimeofday(&finish_time_val, NULL);

    timeval interval;
    timeval_subtract(&interval, &finish_time_val, &start_time_val);

    double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

    std::cout << "Copied " << memory_region_size / 1024 / 1024 << " Mbytes of memory from vector to vector in "
              << used_time << " seconds" << std::endl
              << std::endl;
}

template <typename T> void do_parallel_memory_copy(const T& counter_map) {
    // Create std::vectors from map
    std::array<std::vector<std::pair<uint32_t, subnet_counter_t>>, 4> source_vectors;

    for (auto& current_vector : source_vectors) {
        current_vector.reserve(counter_map.size());

        for (auto& itr : counter_map) {
            current_vector.push_back(itr);
        }
    }

    // Target vector to receive copy
    std::array<std::vector<std::pair<uint32_t, subnet_counter_t>>, 4> target_vectors;


    std::thread t1([&]() { try_to_copy_vector_to_vector_memory_copy(target_vectors[0], source_vectors[0]); });

    std::thread t2([&]() { try_to_copy_vector_to_vector_memory_copy(target_vectors[1], source_vectors[1]); });

    std::thread t3([&]() { try_to_copy_vector_to_vector_memory_copy(target_vectors[2], source_vectors[2]); });

    std::thread t4([&]() { try_to_copy_vector_to_vector_memory_copy(target_vectors[3], source_vectors[3]); });


    t1.join();
    t2.join();
    t3.join();
    t4.join();

    for (auto& current_vector : source_vectors) {
        current_vector.clear();
    }

    for (auto& current_vector : target_vectors) {
        current_vector.clear();
    }
}

// Unconditional check (unlike assert(), which is compiled out under -DNDEBUG / Release builds,
// which would silently disable every validity check below). Records failures so the process can
// exit non-zero.
static int g_check_failures = 0;
static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "  [CHECK FAILED] " << message << std::endl;
        g_check_failures++;
    }
}

// Current resident set size of this process in MB (actual, measured), or -1 if unavailable.
static long current_rss_mb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %ld kB", &kb) == 1) return kb / 1024;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    // Source selection:
    //   argv[1] = path to a real production dump (optional). If absent or unreadable we
    //             generate a synthetic, deliberately MULTI-SHARD source, so the all-shards
    //             iteration below is genuinely exercised (shard 0 does NOT hold everything).
    //   argv[2] = synthetic source shard count (default 8)
    //   argv[3] = synthetic host count        (default 2000000)
    //   argv[4] = timing iterations           (default 3; the original used an infinite loop)
    const char* dump_path     = (argc > 1 && argv[1][0] != '\0') ? argv[1] : nullptr;
    size_t synth_src_shards   = (argc > 2) ? (size_t)std::strtoul(argv[2], nullptr, 10) : 8;
    long   synth_hosts        = (argc > 3) ? std::strtol(argv[3], nullptr, 10) : 2000000;
    int    iterations         = (argc > 4) ? std::atoi(argv[4]) : 3;
    if (synth_src_shards == 0) synth_src_shards = 1;
    if (iterations < 1) iterations = 1;

    bool loaded = false;
    if (dump_path) {
        std::cout << "Load structure from disk dump: " << dump_path << std::endl;
        time_t start_time = 0;
        time(&start_time);
        try {
            std::ifstream deserialize_stream(dump_path);
            boost::archive::binary_iarchive input_archive(deserialize_stream);

            input_archive >> BOOST_SERIALIZATION_NVP(ipv4_host_counters);
            loaded = true;

            time_t finish_time = 0;
            time(&finish_time);
            std::cout << "Loaded traffic counters from disk in " << finish_time - start_time << " seconds"
                      << std::endl;
        } catch (const std::exception& e) {
            std::cout << "Could not load dump (" << e.what() << "); falling back to synthetic data" << std::endl;
        } catch (...) {
            std::cout << "Could not load dump; falling back to synthetic data" << std::endl;
        }
    }

    if (!loaded) {
        std::cout << "Generating synthetic MULTI-SHARD source: " << synth_hosts << " hosts across "
                  << synth_src_shards << " source shards" << std::endl;
        // Make the SOURCE span multiple internal shards, so shards[0] is NOT the whole set.
        ipv4_host_counters.configure_sharding(synth_src_shards, 1);
        simple_packet_t current_packet;
        current_packet.protocol = 6;
        std::mt19937 rng(123456789u);
        for (long i = 0; i < synth_hosts; i++) {
            uint32_t ip           = rng(); // diverse keys spread over both source shards and partitions
            current_packet.length = 40 + (ip % 1400);
            ipv4_host_counters.increment_incoming_counters_for_key(ip, current_packet, 1 + (ip % 5),
                                                                   current_packet.length);
        }
    }

    // ---- Source statistics across EVERY shard (not just shard 0) ------------
    size_t src_total = 0, src_buckets = 0, populated_src_shards = 0;
    for (auto& s : ipv4_host_counters.shards) {
        std::lock_guard<std::mutex> g(s->mutex);
        src_total += s->counter_map.size();
        src_buckets += s->counter_map.bucket_count();
        if (!s->counter_map.empty()) populated_src_shards++;
    }

    std::cout << "Source container shards: " << ipv4_host_counters.shards.size()
              << " (populated: " << populated_src_shards << ")" << std::endl;
    std::cout << "Total hosts across all source shards: " << src_total << std::endl;
    std::cout << "Total bucket count across all source shards: " << src_buckets << std::endl;
    std::cout << "Estimated counter-map memory (lower bound: keys+values only, excludes bucket/allocator "
                 "overhead): "
              << (src_total * (sizeof(subnet_counter_t) + sizeof(uint32_t))) / 1024 / 1024 << " Mb" << std::endl;
    long rss = current_rss_mb();
    if (rss >= 0) {
        std::cout << "Actual process RSS (measured): " << rss << " Mb" << std::endl;
    } else {
        std::cout << "Actual process RSS (measured): unavailable on this platform" << std::endl;
    }

    double speed_calc_period        = 1;
    double average_calculation_time = 60;

    // ---- Redistribute hosts into 32 independent partition-containers --------
    // Ownership model: partition index = fast_ntoh(ip) % 32 selects which of the 32 INDEPENDENT
    // counter containers owns a host; each container is swept by its own thread below (this is the
    // benchmark's 32-way sharding experiment). We iterate EVERY source shard and insert through the
    // public, invariant-respecting API (modify_counter_for_key routes the key via shard_for()),
    // instead of poking .shards[0] directly.
    std::cout << "Sharding data into 32 partitions" << std::endl;
    const int sharding_value = 32;

    std::array<abstract_subnet_counters_t<uint32_t, subnet_counter_t>, sharding_value> speed_counters;

    size_t redistributed = 0;
    for (auto& s : ipv4_host_counters.shards) {
        std::lock_guard<std::mutex> g(s->mutex);
        for (const auto& itr : s->counter_map) {
            // NB! ntoh is required: raw big-endian keys would collapse onto a handful of partitions.
            int reminder = fast_ntoh(itr.first) % sharding_value;
            speed_counters[reminder].modify_counter_for_key(
                itr.first, [&itr](subnet_counter_t& dst) { dst = itr.second; });
            redistributed++;
        }
    }

    // ---- Validity assertions (redistribution must be a faithful partition) --
    size_t dst_total            = 0;
    int    populated_partitions = 0;
    for (auto& sc : speed_counters) {
        size_t sz = sc.size();
        dst_total += sz;
        if (sz > 0) populated_partitions++;
    }

    // Placement correctness: every key in partition p must map back to p, and the union of all
    // partitions must equal the source key set (so nothing was dropped or silently duplicated).
    size_t placement_errors = 0, union_count = 0;
    for (int p = 0; p < sharding_value; p++) {
        for (auto& s : speed_counters[p].shards) {
            std::lock_guard<std::mutex> g(s->mutex);
            for (const auto& itr : s->counter_map) {
                if ((int)(fast_ntoh(itr.first) % sharding_value) != p) placement_errors++;
                union_count++;
            }
        }
    }

    std::cout << "\nRedistribution check:" << std::endl;
    std::cout << "  source entries          : " << src_total << std::endl;
    std::cout << "  entries iterated/placed : " << redistributed << std::endl;
    std::cout << "  destination entries     : " << dst_total << std::endl;
    std::cout << "  union over partitions   : " << union_count << std::endl;
    std::cout << "  misplaced entries       : " << placement_errors << std::endl;
    std::cout << "  populated partitions    : " << populated_partitions << "/" << sharding_value << std::endl;
    for (int i = 0; i < sharding_value; i++) {
        std::cout << "    partition[" << i << "] size: " << speed_counters[i].size() << std::endl;
    }

    check(redistributed == src_total, "every source host must be visited exactly once");
    check(dst_total == src_total, "no host dropped or duplicated during redistribution");
    check(union_count == src_total, "union of partitions must equal the source key set");
    check(placement_errors == 0, "every host must sit in the partition its key maps to");
    if (src_total >= 1000) {
        check(populated_partitions == sharding_value,
              "a representative dataset should populate all 32 partitions");
    }
    if (g_check_failures != 0) {
        std::cerr << "REDISTRIBUTION VALIDATION FAILED (" << g_check_failures << " check(s))" << std::endl;
        return 1;
    }
    std::cout << "All redistribution checks passed." << std::endl;

    /*
     Lab server has: AMD Ryzen 5 3600 6-Core Processor
     PC: AMD Ryzen 7 5800X 8-Core Processor

     Lab server, 1 thread (old):
        Calculated speed in 3.70434 seconds
        Calculated speed in 3.73092 seconds
        Calculated speed in 3.72158 seconds

     Lab server, 2 threads:
        Calculated speed in 2.93566 seconds
        Calculated speed in 2.94285 seconds
        Calculated speed in 2.92184 seconds

     Lab server, 3 threads:
        Calculated speed in 2.17883 seconds
        Calculated speed in 2.13465 seconds
        Calculated speed in 2.09182 seconds

     Lab server, 4 threads:
        Calculated speed in 1.76031 seconds
        Calculated speed in 1.82056 seconds
        Calculated speed in 1.79133 seconds

      Lab server, 5 threads:
        Calculated speed in 1.47045 seconds
        Calculated speed in 1.47526 seconds
        Calculated speed in 1.41408 seconds

       Lab server, 6 threads (2.7 times better than single threaded!):
        Calculated speed in 1.35194 seconds
        Calculated speed in 1.37153 seconds
        Calculated speed in 1.34529 seconds

     PC, 1 thread (old):
        Calculated speed in 2.80088 seconds
        Calculated speed in 2.80217 seconds
        Calculated speed in 2.80967 seconds

     PC, 4 threads:
        Calculated speed in 1.37763 seconds
        Calculated speed in 1.38572 seconds
        Calculated speed in 1.37288 seconds

     PC, 6 threads:
        Calculated speed in 1.12384 seconds
        Calculated speed in 1.11926 seconds
        Calculated speed in 1.1167 seconds

     PC, 7 threads:
        Calculated speed in 1.02398 seconds
        Calculated speed in 1.0216 seconds
        Calculated speed in 1.01837 seconds

     PC, 8 threads (2.7 times better than single threaded!):
        Calculated speed in 1.03207 seconds
        Calculated speed in 1.01931 seconds
        Calculated speed in 1.0154 seconds

     GCE, c2-standard-16 (16 cores), Intel Cascade Lake, 1 thread (old):
        Calculated speed in 4.60883 seconds
        Calculated speed in 4.61683 seconds
        Calculated speed in 4.60568 seconds

     GCE, c2-standard-16 (16 cores), Intel Cascade Lake, 16 threads, (7.6 times better then single thread!):
        Calculated speed in 0.664504 seconds
        Calculated speed in 0.661836 seconds
        Calculated speed in 0.71273 seconds

    GCE, e2-highcpu-32 (32 cores), Intel Broadwell, 1 thread (old):
        Calculated speed in 5.68188 seconds
        Calculated speed in 5.70301 seconds
        Calculated speed in 5.69111 seconds


     GCE, e2-highcpu-32 (32 cores), Intel Broadwell, 16 threads (9.9 times better then single thread!):
        Calculated speed in 0.562593 seconds
        Calculated speed in 0.565208 seconds
        Calculated speed in 0.559615 seconds

     GCE, e2-highcpu-32 (32 cores), Intel Broadwell, 32 threads (13 times better then single thread!):
        Calculated speed in 0.425343 seconds
        Calculated speed in 0.425999 seconds
        Calculated speed in 0.434782 seconds

     */

    std::cout << "\nRunning " << iterations << " parallel speed-calc iteration(s) over " << sharding_value
              << " partitions" << std::endl;

    for (int iter = 0; iter < iterations; iter++) {
        timeval start_time_val;
        gettimeofday(&start_time_val, NULL);

        std::vector<std::thread> threads;
        threads.reserve(sharding_value);

        for (auto& current_speed_counter : speed_counters) {
            // Capture a POINTER to THIS specific array element, by value. The previous code
            // captured the range-for variable by reference ([&]); since that variable is recreated
            // each iteration, every thread ended up aliasing the same (last) element -- some
            // partitions were swept repeatedly, others never, and the reference outlived its scope.
            abstract_subnet_counters_t<uint32_t, subnet_counter_t>* counter_ptr = &current_speed_counter;
            threads.emplace_back([counter_ptr, speed_calc_period, average_calculation_time]() {
                counter_ptr->recalculate_speed(speed_calc_period, average_calculation_time, nullptr);
            });
        }

        // Wait threads to finish
        for (auto& current_thread : threads) {
            current_thread.join();
        }

        timeval finish_time_val;
        gettimeofday(&finish_time_val, NULL);

        timeval interval;
        timeval_subtract(&interval, &finish_time_val, &start_time_val);

        double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

        std::cout << "Calculated speed in " << used_time << " seconds" << std::endl << std::endl;
    }

    /*
    while (true) {
        timeval start_time_val;
        gettimeofday(&start_time_val, NULL);

        ipv4_host_counters.recalculate_speed(speed_calc_period, average_calculation_time, nullptr);

        timeval finish_time_val;
        gettimeofday(&finish_time_val, NULL);

        timeval interval;
        timeval_subtract(&interval, &finish_time_val, &start_time_val);

        double used_time = (double)interval.tv_sec + (double)interval.tv_usec / 1000000;

        std::cout << "Calculated speed in " << used_time  << " seconds" << std::endl << std::endl;
    }
    */

    return 0;
}
