#pragma once

// Sharded per-key counters.
//
// Storage is split into N independent shards, each a {counter_map, average_speed_map,
// mutex}. A key is routed to exactly one shard by hash(key) % N, so a key's live
// counters, its average and its math always stay together. This removes the single
// global lock that previously serialised (a) every per-packet increment and (b) the
// once-per-second recalc sweep. Behaviour is identical to the single-map version:
// each key's accounting and EWMA are unchanged; only the lock is partitioned.
//
// Shard count defaults to 1 (byte-identical to the original single-map behaviour).
// Override the default with -DFNM_DEFAULT_SHARDS=N or construct with a shard count.

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

#include "speed_counters.hpp"

#include <boost/serialization/version.hpp>
#if BOOST_VERSION / 100000 == 1 && BOOST_VERSION / 100 % 1000 == 74
#include <boost/serialization/library_version_type.hpp>
#endif
#include <boost/serialization/unordered_map.hpp>

#ifndef FNM_DEFAULT_SHARDS
#define FNM_DEFAULT_SHARDS 1
#endif

template <typename T, typename Counter, typename UM = std::unordered_map<T, Counter>> class abstract_subnet_counters_t {
    public:
    // One independent partition. unique_ptr because std::mutex is non-movable.
    struct shard_t {
        UM counter_map;
        UM average_speed_map;
        std::mutex mutex;
    };

    using Hasher = typename UM::hasher;

    size_t num_shards;
    std::vector<std::unique_ptr<shard_t>> shards;

    // Number of worker threads for the recalc sweep. 1 = sequential (identical to the
    // original single-threaded behaviour). >1 fans the shards across threads. Enabling
    // this with side-effecting callbacks requires those callbacks to be thread-safe;
    // the pure sweep (no callbacks) is always safe.
    size_t recalc_threads = 1;

    explicit abstract_subnet_counters_t(size_t shard_count = FNM_DEFAULT_SHARDS)
        : num_shards(shard_count == 0 ? 1 : shard_count) {
        shards.reserve(num_shards);
        for (size_t i = 0; i < num_shards; i++) shards.push_back(std::make_unique<shard_t>());
    }

    // A key is ALWAYS routed to the same shard, so its per-key math is never split.
    inline shard_t& shard_for(const T& key) {
        return *shards[Hasher{}(key) % num_shards];
    }

    // Reconfigure shard count and recalc parallelism. MUST be called only at startup,
    // before the counters are populated or touched by any other thread (it resets the
    // underlying storage). shard_count/threads of 0 are treated as 1 (safe default).
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

    // The daemon does not persist these counters; serialization is used only by the
    // developer performance test. With a binary archive (which ignores field names) and
    // the default single shard, this emits the same {counter_map, average_speed_map}
    // sequence as the original, so legacy dumps still load. Multi-shard dumps are only
    // interchangeable with a build using the same shard count.
    template <class Archive> void serialize(Archive& ar, [[maybe_unused]] const unsigned int version) {
        for (auto& s : shards) {
            ar& BOOST_SERIALIZATION_NVP(s->counter_map);
            ar& BOOST_SERIALIZATION_NVP(s->average_speed_map);
        }
    }

    // ---- per-packet increments: lock ONLY the key's shard ------------------
    void increment_outgoing_counters_for_key(const T& key,
                                             const simple_packet_t& current_packet,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        Counter& counters = s.counter_map[key];
        increment_outgoing_counters(counters, current_packet, sampled_number_of_packets, sampled_number_of_bytes);
    }

    template <size_t N>
    void increment_outgoing_counters_for_key(const T& key,
                                             const std::array<bool, N>& matched_indexes,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        Counter& counters = s.counter_map[key];

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
        Counter& counters = s.counter_map[key];
        increment_incoming_counters(counters, current_packet, sampled_number_of_packets, sampled_number_of_bytes);
    }

    template <size_t N>
    void increment_incoming_counters_for_key(const T& key,
                                             const std::array<bool, N>& matched_indexes,
                                             uint64_t sampled_number_of_packets,
                                             uint64_t sampled_number_of_bytes) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        Counter& counters = s.counter_map[key];

        extern time_t current_inaccurate_time;
        counters.last_update_time = current_inaccurate_time;

        for (std::size_t current_index = 0; current_index < counters.flexible_counters.size(); current_index++) {
            if (matched_indexes[current_index]) {
                counters.flexible_counters[current_index].in_packets += sampled_number_of_packets;
                counters.flexible_counters[current_index].in_bytes += sampled_number_of_bytes;
            }
        }
    }

    // ---- accessors (replace former direct .counter_map / .average_speed_map access) ----

    // Lock the key's shard and let the caller create/mutate its counter in place.
    template <typename F>
    void modify_counter_for_key(const T& key, F&& fn) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        fn(s.counter_map[key]);
    }

    // Pre-create zeroed entries for a key in both maps (startup pre-allocation).
    void preallocate_key(const T& key, const Counter& zero_value) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        s.counter_map[key]       = zero_value;
        s.average_speed_map[key] = zero_value;
    }

    size_t size() {
        size_t total = 0;
        for (auto& s : shards) { std::lock_guard<std::mutex> g(s->mutex); total += s->counter_map.size(); }
        return total;
    }

    size_t average_speed_size() {
        size_t total = 0;
        for (auto& s : shards) { std::lock_guard<std::mutex> g(s->mutex); total += s->average_speed_map.size(); }
        return total;
    }

    // ---- snapshot / query: merge across shards -----------------------------
    void get_all_average_speed_elements(UM& copy_of_average_speed_map) {
        copy_of_average_speed_map.clear();
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            copy_of_average_speed_map.insert(s->average_speed_map.begin(), s->average_speed_map.end());
        }
    }

    uint64_t purge_old_data(unsigned int automatic_data_cleanup_threshold) {
        uint64_t removed = 0;
        time_t current_time = 0;
        time(&current_time);
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            std::vector<T> keys_to_remove;
            for (auto itr = s->counter_map.begin(); itr != s->counter_map.end(); ++itr) {
                if ((int64_t)itr->second.last_update_time < int64_t((int64_t)current_time - (int64_t)automatic_data_cleanup_threshold)) {
                    keys_to_remove.push_back(itr->first);
                }
            }
            for (const auto& key : keys_to_remove) {
                s->counter_map.erase(key);
                s->average_speed_map.erase(key);
            }
            removed += keys_to_remove.size();
        }
        return removed;
    }

    // ---- the 1s sweep: each shard swept under its own lock -----------------
    // Shards are swept sequentially when recalc_threads<=1 (behaviour-identical). With
    // more threads, shards are fanned across workers; the per-shard body is independent.
    void recalculate_speed(double speed_calc_period,
                           double average_calculation_time,
                           std::function<void(const T&, const Counter&)> speed_check_callback      = nullptr,
                           std::function<void(const T&, Counter&, double)> new_speed_calc_callback = nullptr) {
        double exp_power_subnet = -speed_calc_period / average_calculation_time;
        double exp_value_subnet = exp(exp_power_subnet);

        // Per-shard work. Shards are independent (separate maps + lock), so this body
        // is safe to run on many shards concurrently; only the callbacks, if they touch
        // shared state, need to be thread-safe.
        auto process_shard = [&](shard_t& s) {
            std::lock_guard<std::mutex> lock_guard(s.mutex);
            for (auto itr = s.counter_map.begin(); itr != s.counter_map.end(); ++itr) {
                const T& current_key      = itr->first;
                Counter& traffic_counters = itr->second;

                Counter new_speed_element;
                build_speed_counters_from_packet_counters(new_speed_element, traffic_counters, speed_calc_period);

                if (new_speed_calc_callback != nullptr) {
                    new_speed_calc_callback(current_key, new_speed_element, speed_calc_period);
                }

                Counter& current_average_speed_element = s.average_speed_map[current_key];
                build_average_speed_counters_from_speed_counters(current_average_speed_element, new_speed_element, exp_value_subnet);

                traffic_counters.zeroify();

                if (speed_check_callback != nullptr) {
                    speed_check_callback(current_key, current_average_speed_element);
                }
            }
        };

        if (recalc_threads <= 1 || num_shards <= 1) {
            // Sequential sweep — identical to the original behaviour.
            for (auto& sp : shards) process_shard(*sp);
        } else {
            // Parallel sweep: workers pull shards off a shared counter until exhausted.
            // A throwing callback must not call std::terminate in a worker thread, so we
            // capture the first exception and rethrow after joining — matching the
            // sequential path, where an exception propagates out of recalculate_speed.
            size_t nt = std::min(recalc_threads, num_shards);
            std::atomic<size_t> next_shard{ 0 };
            std::exception_ptr first_error;
            std::mutex error_mutex;

            // Each worker (and the current thread, in the fallback below) runs this: pull
            // shards off next_shard until none remain. Any worker count >= 1 drains every
            // shard, so losing some workers only costs parallelism, never coverage.
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

            // Creating a thread can fail under resource exhaustion (pthread_create EAGAIN
            // on RLIMIT_NPROC / stack memory), which throws std::system_error from the
            // std::thread constructor. If that propagated here, the workers vector would be
            // destroyed with still-joinable threads and call std::terminate() — turning a
            // transient shortage into a daemon crash. Instead we stop spawning and keep the
            // workers we already have; if none were created we sweep inline. Correctness is
            // preserved either way (every shard is still processed exactly once).
            try {
                for (size_t t = 0; t < nt; t++) {
                    workers.emplace_back(worker_body);
                }
            } catch (const std::system_error&) {
                // Degrade gracefully to however many workers we managed to start.
            }

            if (workers.empty()) {
                // Not a single worker could be started: do the whole sweep on this thread.
                worker_body();
            }

            for (auto& w : workers) w.join();
            if (first_error) std::rethrow_exception(first_error);
        }
    }

    void get_all_non_zero_average_speed_elements_as_pairs(std::vector<std::pair<T, Counter>>& all_elements) {
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            for (auto itr = s->average_speed_map.begin(); itr != s->average_speed_map.end(); ++itr) {
                if (itr->second.is_zero()) continue;
                all_elements.push_back(std::make_pair(itr->first, itr->second));
            }
        }
    }

    void get_sorted_average_speed(std::vector<std::pair<T, Counter>>& vector_for_sort,
                                  const attack_detection_threshold_type_t& sorter_type,
                                  const attack_detection_direction_type_t& sort_direction) {
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            std::copy(s->average_speed_map.begin(), s->average_speed_map.end(), std::back_inserter(vector_for_sort));
        }
        std::sort(vector_for_sort.begin(), vector_for_sort.end(),
                  TrafficComparatorClass<std::pair<T, Counter>>(sort_direction, sorter_type));
    }

    bool get_average_speed(const T& key, Counter& average_speed_element) {
        shard_t& s = shard_for(key);
        std::lock_guard<std::mutex> lock_guard(s.mutex);
        auto average_speed_itr = s.average_speed_map.find(key);
        if (average_speed_itr == s.average_speed_map.end()) return false;
        average_speed_element = average_speed_itr->second;
        return true;
    }

    void get_top_k_average_speed(std::vector<std::pair<T, Counter>>& vector_for_sort,
                                 const attack_detection_threshold_type_t& sorter_type,
                                 const attack_detection_direction_type_t& sort_direction) {
        std::vector<std::pair<T, Counter>> merged;
        for (auto& s : shards) {
            std::lock_guard<std::mutex> lock_guard(s->mutex);
            std::copy(s->average_speed_map.begin(), s->average_speed_map.end(), std::back_inserter(merged));
        }
        std::partial_sort_copy(merged.begin(), merged.end(), vector_for_sort.begin(), vector_for_sort.end(),
                               TrafficComparatorClass<std::pair<T, Counter>>(sort_direction, sorter_type));
    }
};
