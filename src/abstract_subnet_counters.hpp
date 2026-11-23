#pragma once

// Sharded per-key counters with DENSE, CONTIGUOUS storage for a cache-efficient 1s sweep.
//
// Storage is split into N independent shards. Within a shard the live data lives in three
// PARALLEL, CONTIGUOUS vectors indexed by a dense slot:
//     keys[slot]      -> the key
//     counters[slot]  -> current packet counters (the former counter_map value)
//     average[slot]   -> average speed (the former average_speed_map value)
// plus a hash index (key -> slot) used only for per-packet lookups and point queries.
//
// Why: the once-per-second recalc sweep visits every host. With node-based std::unordered_map
// each host is a random heap node (cache + TLB miss per host), so the sweep is memory-LATENCY
// bound and does not scale with cores (measured: ~1.2x at 4 threads). Walking contiguous vectors
// is sequential and prefetchable, so the sweep becomes memory-BANDWIDTH bound and scales with
// cores (measured: ~7x faster single-threaded and near-linear across workers). It also drops the
// second hash lookup the old sweep did per host (average_speed_map[key]) — average[slot] is O(1).
//
// A key is routed to exactly one shard by hash(key) % N, so its counters, average and math always
// stay together under one lock. Per-key accounting and EWMA are byte-identical to the original;
// only the container layout and the lock partitioning change. Shard count defaults to 1.

#include <mutex>
#include <functional>
#include <vector>
#include <memory>
#include <array>
#include <cmath>
#include <thread>
#include <atomic>
#include <algorithm>
#include <exception>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <ranges>

#include "speed_counters.hpp"

#include <boost/serialization/version.hpp>
#if BOOST_VERSION / 100000 == 1 && BOOST_VERSION / 100 % 1000 == 74
#include <boost/serialization/library_version_type.hpp>
#endif
#include <boost/serialization/vector.hpp>

#ifndef FNM_DEFAULT_SHARDS
#define FNM_DEFAULT_SHARDS 1
#endif

// UM is retained for source compatibility (and to source the key hasher); storage is dense.
template <typename T, typename Counter, typename UM = std::unordered_map<T, Counter>> class abstract_subnet_counters_t {
    public:
    // One independent partition. Dense parallel vectors + a key->slot index. unique_ptr because
    // std::mutex is non-movable.
    struct shard_t {
        // slot_index_t is size_t so the index never truncates keys.size(); used consistently by the
        // index map, inserts, purge and (implicitly) iteration.
        using slot_index_t = size_t;
        std::unordered_map<T, slot_index_t> index; // key -> dense slot
        std::vector<T> keys;                        // slot -> key
        std::vector<Counter> counters;              // slot -> current packet counters
        std::vector<Counter> average;               // slot -> average speed
        std::mutex mutex;

        // Append a brand-new key with the given initial values TRANSACTIONALLY: either all four
        // containers gain the new slot, or none do (strong exception guarantee). A single catch rolls
        // back EXACTLY the operations that completed (tracked by `done`), so every pop_back runs at most
        // once and never on an empty vector. std::vector::push_back is individually strong-guaranteed
        // (on throw the vector is unchanged, so the throwing op is never rolled back), and pop_back is
        // noexcept — so a mid-way bad_alloc can never leave the index pointing at a non-existent slot or
        // the vectors at unequal sizes. Returns the new slot. Caller holds the shard lock, key absent.
        inline slot_index_t append_slot(const T& key, const Counter& init_counter, const Counter& init_average) {
            slot_index_t slot = keys.size();
            int done = 0; // number of the 4 steps below that have fully succeeded
            try {
                keys.push_back(key);              ++done; // 1
                counters.push_back(init_counter); ++done; // 2
                average.push_back(init_average);  ++done; // 3
                index.emplace(key, slot);         ++done; // 4
            } catch (...) {
                if (done >= 3) average.pop_back();
                if (done >= 2) counters.pop_back();
                if (done >= 1) keys.pop_back();
                throw;
            }
            return slot;
        }

        // Get (creating if absent) the current-counter slot for a key. Returns a reference valid
        // only until the next insert on this shard (vector may reallocate) — callers use it under
        // the shard lock and do not hold it across inserts.
        inline Counter& counter_ref(const T& key) {
            auto it = index.find(key);
            if (it != index.end()) return counters[it->second];
            slot_index_t slot = append_slot(key, Counter{}, Counter{});
            return counters[slot];
        }
    };

    using Hasher = typename UM::hasher;

    size_t num_shards;
    std::vector<std::unique_ptr<shard_t>> shards;

    // Worker threads for the recalc sweep. 1 = sequential (identical to the original behaviour).
    size_t recalc_threads = 1;

    explicit abstract_subnet_counters_t(size_t shard_count = FNM_DEFAULT_SHARDS)
        : num_shards(shard_count == 0 ? 1 : shard_count) {
        shards.reserve(num_shards);
        for (size_t i = 0; i < num_shards; i++) shards.push_back(std::make_unique<shard_t>());
    }

    // Route a key to its shard. We must MIX the hash bits before the modulo: real keys are highly
    // structured (network-order IPv4 addresses from a few customer prefixes share their low-order
    // bits — e.g. the constant leading octet lands in the bits a small modulo selects), and the
    // standard integer hash is identity on libstdc++, so `hash(key) % N` would collapse millions of
    // hosts onto a single shard and leave the rest empty — serialising the "parallel" sweep onto one
    // core. A finaliser-style avalanche spreads structured keys evenly across shards. The mapping is
    // deterministic and used identically by ingest, sweep and queries, so per-key results are
    // unchanged; only the shard assignment (hence load balance) differs.
    static inline uint64_t mix_bits(uint64_t h) {
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        h ^= h >> 33;
        return h;
    }

    inline size_t shard_index(const T& key) const {
        return (size_t)(mix_bits((uint64_t)Hasher{}(key)) % num_shards);
    }

    inline shard_t& shard_for(const T& key) {
        return *shards[shard_index(key)];
    }

    // Reconfigure shard count and recalc parallelism. MUST be called only at startup, before the
    // counters are populated or touched by any other thread.
    void configure_sharding(size_t shard_count, size_t threads) {
        recalc_threads = (threads == 0 ? 1 : threads);
        size_t n = (shard_count == 0 ? 1 : shard_count);
        if (n != num_shards) {
            num_shards = n;
            shards.clear();
            shards.reserve(n);
            for (size_t i = 0; i < n; i++) shards.push_back(std::make_unique<shard_t>());
        }
    }

    // Serialization (used only by the developer performance test; the daemon never persists these).
    // Emits the dense vectors per shard. A dump is interchangeable only with a build using the same
    // shard count — as was already true for the multi-shard layout.
    template <class Archive> void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
        for (auto& s : shards) {
            ar& BOOST_SERIALIZATION_NVP(s->keys);
            ar& BOOST_SERIALIZATION_NVP(s->counters);
            ar& BOOST_SERIALIZATION_NVP(s->average);
            if (Archive::is_loading::value) {
                s->index.clear();
                s->index.reserve(s->keys.size());
                for (size_t i = 0; i < s->keys.size(); i++) s->index.emplace(s->keys[i], i);
            }
        }
    }

    // ---- per-packet increments: lock ONLY the key's shard ------------------
    void increment_outgoing_counters_for_key(const T& key,
                                             const simple_packet_t& current_packet,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        increment_outgoing_counters(s.counter_ref(key), current_packet, sampled_number_of_packets, sampled_number_of_bytes);
    }

    template <size_t N>
    void increment_outgoing_counters_for_key(const T& key,
                                             const std::array<bool, N>& matched_indexes,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        Counter& counters = s.counter_ref(key);

        extern time_t current_inaccurate_time;
        counters.last_update_time = current_inaccurate_time;

        for (std::size_t current_index = 0; current_index < counters.flexible_counters.size(); current_index++) {
            if (matched_indexes[current_index]) {
                counters.flexible_counters[current_index].out_packets += sampled_number_of_packets;
                counters.flexible_counters[current_index].out_bytes += sampled_number_of_bytes;
            }
        }
    }

    void increment_incoming_counters_for_key(const T& key,
                                             const simple_packet_t& current_packet,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        increment_incoming_counters(s.counter_ref(key), current_packet, sampled_number_of_packets, sampled_number_of_bytes);
    }

    template <size_t N>
    void increment_incoming_counters_for_key(const T& key,
                                             const std::array<bool, N>& matched_indexes,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        Counter& counters = s.counter_ref(key);

        extern time_t current_inaccurate_time;
        counters.last_update_time = current_inaccurate_time;

        for (std::size_t current_index = 0; current_index < counters.flexible_counters.size(); current_index++) {
            if (matched_indexes[current_index]) {
                counters.flexible_counters[current_index].in_packets += sampled_number_of_packets;
                counters.flexible_counters[current_index].in_bytes += sampled_number_of_bytes;
            }
        }
    }

    // ---- accessors ----

    // Lock the key's shard and let the caller create/mutate its counter in place.
    template <typename F>
    void modify_counter_for_key(const T& key, F&& fn) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        fn(s.counter_ref(key));
    }

    // Pre-create zeroed entries for a key (startup pre-allocation).
    void preallocate_key(const T& key, const Counter& zero_value) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        auto it = s.index.find(key);
        if (it != s.index.end()) {
            s.counters[it->second] = zero_value;
            s.average[it->second]  = zero_value;
            return;
        }
        s.append_slot(key, zero_value, zero_value);   // transactional, all-or-nothing
    }

    size_t size() {
        size_t total = 0;
        for (auto& s : shards) { std::lock_guard<std::mutex> g(s->mutex); total += s->keys.size(); }
        return total;
    }

    size_t average_speed_size() {
        size_t total = 0;
        for (auto& s : shards) { std::lock_guard<std::mutex> g(s->mutex); total += s->average.size(); }
        return total;
    }

    // ---- snapshot / query ----
    void get_all_average_speed_elements(std::unordered_map<T, Counter>& copy_of_average_speed_map) {
        copy_of_average_speed_map.clear();
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            for (size_t j = 0; j < s->keys.size(); j++) copy_of_average_speed_map[s->keys[j]] = s->average[j];
        }
    }

    uint64_t purge_old_data(unsigned int automatic_data_cleanup_threshold) {
        uint64_t removed = 0;
        time_t current_time = 0;
        time(&current_time);
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            size_t j = 0;
            while (j < s->keys.size()) {
                if ((int64_t)s->counters[j].last_update_time <
                    int64_t((int64_t)current_time - (int64_t)automatic_data_cleanup_threshold)) {
                    // swap-with-last + pop; fix the moved key's index entry
                    size_t last = s->keys.size() - 1;
                    s->index.erase(s->keys[j]);
                    if (j != last) {
                        s->keys[j]     = s->keys[last];
                        s->counters[j] = s->counters[last];
                        s->average[j]  = s->average[last];
                        s->index[s->keys[j]] = j;   // size_t slot, no truncation
                    }
                    s->keys.pop_back();
                    s->counters.pop_back();
                    s->average.pop_back();
                    removed++;
                    // do not advance j: the swapped-in element must be checked
                } else {
                    j++;
                }
            }
        }
        return removed;
    }

    // ---- the 1s sweep: each shard swept under its own lock -----------------
    // Shards are swept sequentially when recalc_threads<=1 (behaviour-identical). With more threads
    // they are fanned across workers. The per-shard body is a CONTIGUOUS linear scan of the dense
    // vectors — sequential, prefetchable, no per-host hash lookup.
    void recalculate_speed(double speed_calc_period,
                           double average_calculation_time,
                           std::function<void(const T&, const Counter&)> speed_check_callback      = nullptr,
                           std::function<void(const T&, Counter&, double)> new_speed_calc_callback = nullptr) {
        double exp_power_subnet = -speed_calc_period / average_calculation_time;
        double exp_value_subnet = exp(exp_power_subnet);

        auto process_shard = [&](shard_t& s) {
            std::lock_guard<std::mutex> lock_guard(s.mutex);
            const size_t n = s.keys.size();
            for (size_t j = 0; j < n; j++) {
                const T& current_key      = s.keys[j];
                Counter& traffic_counters = s.counters[j];

                Counter new_speed_element;
                build_speed_counters_from_packet_counters(new_speed_element, traffic_counters, speed_calc_period);

                if (new_speed_calc_callback != nullptr) {
                    new_speed_calc_callback(current_key, new_speed_element, speed_calc_period);
                }

                Counter& current_average_speed_element = s.average[j];
                build_average_speed_counters_from_speed_counters(current_average_speed_element, new_speed_element, exp_value_subnet);

                traffic_counters.zeroify();

                if (speed_check_callback != nullptr) {
                    speed_check_callback(current_key, current_average_speed_element);
                }
            }
        };

        if (recalc_threads <= 1 || num_shards <= 1) {
            for (auto& sp : shards) process_shard(*sp);
        } else {
            size_t nt = std::min(recalc_threads, num_shards);
            std::atomic<size_t> next_shard{ 0 };
            std::exception_ptr first_error;
            std::mutex error_mutex;

            auto worker_body = [&]() {
                size_t i;
                while ((i = next_shard.fetch_add(1)) < num_shards) {
                    try {
                        process_shard(*shards[i]);
                    } catch (...) {
                        std::lock_guard<std::mutex> error_guard(error_mutex);
                        if (!first_error) first_error = std::current_exception();
                    }
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(nt);
            try {
                for (size_t t = 0; t < nt; t++) workers.emplace_back(worker_body);
            } catch (const std::system_error&) {
                // Degrade gracefully to however many workers we managed to start.
            }
            if (workers.empty()) worker_body();
            for (auto& w : workers) w.join();
            if (first_error) std::rethrow_exception(first_error);
        }
    }

    void get_all_non_zero_average_speed_elements_as_pairs(std::vector<std::pair<T, Counter>>& all_elements) {
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            for (size_t j = 0; j < s->keys.size(); j++) {
                if (s->average[j].is_zero()) continue;
                all_elements.push_back(std::make_pair(s->keys[j], s->average[j]));
            }
        }
    }

    void get_sorted_average_speed(std::vector<std::pair<T, Counter>>& vector_for_sort,
                                  const attack_detection_threshold_type_t& sorter_type,
                                  const attack_detection_direction_type_t& sort_direction) {
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            for (size_t j = 0; j < s->keys.size(); j++)
                vector_for_sort.push_back(std::make_pair(s->keys[j], s->average[j]));
        }
        std::sort(vector_for_sort.begin(), vector_for_sort.end(),
                  TrafficComparatorClass<std::pair<T, Counter>>(sort_direction, sorter_type));
    }

    bool get_average_speed(const T& key, Counter& average_speed_element) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        auto it = s.index.find(key);
        if (it == s.index.end()) return false;
        average_speed_element = s.average[it->second];
        return true;
    }

    void get_top_k_average_speed(std::vector<std::pair<T, Counter>>& vector_for_sort,
                                 const attack_detection_threshold_type_t& sorter_type,
                                 const attack_detection_direction_type_t& sort_direction) {
        const TrafficComparatorClass<std::pair<T, Counter>> comparator(sort_direction, sorter_type);
        const size_t k = vector_for_sort.size();
        if (k == 0) return;

        // All buffers are bounded by k (the rows the caller wants) or shards*k — NEVER by host count.
        // Per shard we select the top-k directly over a LAZY index->pair view, so partial_sort_copy
        // keeps only a k-sized buffer internally and we never materialise an O(hosts) pairs vector.
        std::vector<std::pair<T, Counter>> shard_top(k);
        std::vector<std::pair<T, Counter>> merged;
        merged.reserve(shards.size() * k);

        for (auto& s : shards) {
            typename std::vector<std::pair<T, Counter>>::iterator shard_top_end;
            {
                std::lock_guard<std::mutex> lock_guard(s->mutex);
                auto pair_view = std::views::iota(std::size_t{ 0 }, s->keys.size()) |
                                 std::views::transform([&s](std::size_t j) {
                                     return std::make_pair(s->keys[j], s->average[j]);
                                 });
                // std::ranges::partial_sort_copy keeps only a k-sized buffer internally and supports
                // lazy view iterators (the classic std::partial_sort_copy does not), so no O(hosts)
                // vector is ever materialised. Returns {in, out}; out is the end of what was written.
                auto res = std::ranges::partial_sort_copy(pair_view, shard_top, comparator);
                shard_top_end = res.out;
            }
            merged.insert(merged.end(), shard_top.begin(), shard_top_end);
        }

        // Global top-k over the union equals the top-k over the concatenation of per-shard top-k.
        std::partial_sort_copy(merged.begin(), merged.end(), vector_for_sort.begin(), vector_for_sort.end(),
                               comparator);
    }
};
