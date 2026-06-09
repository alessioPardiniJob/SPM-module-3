#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <ska/flat_hash_map.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include "utils/utils.hpp"
#include <thread>
#include <atomic>
#include "utils/affinity.hpp"
#include <cstring>
#include <omp.h>
#include <numeric>
#include <random>


// ------------------------------------------------------------
// Omp Schedule
// ------------------------------------------------------------
enum class OmpSchedule {
    STATIC,
    DYNAMIC,
    GUIDED,
    AUTO
};
inline void set_schedule(OmpSchedule s, int chunk_size)
{
    switch (s) {
        case OmpSchedule::STATIC:
            omp_set_schedule(omp_sched_static, chunk_size);
            break;
        case OmpSchedule::DYNAMIC:
            omp_set_schedule(omp_sched_dynamic, chunk_size);
            break;
        case OmpSchedule::GUIDED:
            omp_set_schedule(omp_sched_guided, chunk_size);
            break;
        case OmpSchedule::AUTO:
            omp_set_schedule(omp_sched_auto, 0);
            break;
    }
}
// ------------------------------------------------------------
// Record definition
// ------------------------------------------------------------
//
// For this reference implementation we only store the key.
// You may extend the record with a payload in later versions if desired.
//
struct Record {
    std::uint64_t key{};
};

// ------------------------------------------------------------
// Utility: command-line parsing
// ------------------------------------------------------------
static bool read_arg_u64(int argc, char** argv, const std::string& name, std::uint64_t& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) {
            out = std::strtoull(argv[i + 1], nullptr, 10);
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------
// Utility: command-line parsing (overloads)
// ------------------------------------------------------------
static bool read_arg_double(int argc, char** argv, const std::string& name, double& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) {
            out = std::stod(argv[i + 1]);
            return true;
        }
    }
    return false;
}


// ------------------------------------------------------------
// Utility: command-line parsing (overloads)
// ------------------------------------------------------------
static bool read_arg_string(int argc, char** argv, const std::string& name, std::string& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) {
            out = argv[i + 1];
            return true;
        }
    }
    return false;
}


// ------------------------------------------------------------
// Utility: usage message
// ------------------------------------------------------------
static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " -nr NR -ns NS -seed SEED -max-key K -p P\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n"
        << "  -sigma      Skew factor (0.0 = uniform, 1.0 = fully skewed)\n"
        << "  -subset-size Size of the hot subset (0 = all partitions)\n";;
}
static bool is_power_of_two(std::uint32_t x) {
    return x != 0 && (x & (x - 1U)) == 0;
}

// ------------------------------------------------------------
// Deterministic pseudo-random generation
// ------------------------------------------------------------
//
// We use splitmix64 to generate reproducible keys and also for checksum.
// https://rosettacode.org/wiki/Pseudo-random_numbers/Splitmix64
//
// splitmix64_next is used as a deterministic pseudo-random generator step,
// while splitmix64 is used as a stateless 64-bit mixing function for checksums. 
//
static inline std::uint64_t splitmix64_mix(std::uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}
static inline std::uint64_t splitmix64(std::uint64_t x) {
    return splitmix64_mix(x + 0x9e3779b97f4a7c15ULL);
}
static inline std::uint64_t splitmix64_next(std::uint64_t& state) {
    state += 0x9e3779b97f4a7c15ULL;
    return splitmix64_mix(state);
}



// ------------------------------------------------------------
// Relation generation with configurable skewness
// ------------------------------------------------------------
static std::vector<Record> generate_relation(
    std::size_t n, 
    std::uint64_t seed, 
    std::uint64_t max_key,
    std::uint32_t num_partitions,        
    double skew_factor = 0.0,             // sigma (0.0 = totally uniform, 1.0 = fully skewed)
    std::uint32_t target_subset_size = 0, // M (size of the hot subset of partitions)
    double crest_shape = 1.0              // theta (1.0 = uniform plateau, >1.0 = sharp crest towards index 0)
) {
    // Output vector
    std::vector<Record> out(n);

    std::uint64_t state = seed;

    // =================================================================
    // SETUP: Creation of the "sparse" subset of hot partitions
    // =================================================================
    std::vector<std::uint32_t> hot_partitions;
    // We only create the hot subset if skew_factor > 0 and target_subset_size > 0
    if (target_subset_size > 0 && skew_factor > 0.0) {
        // We limit the actual subset size to the number of partitions to avoid invalid configurations
        std::uint32_t actual_subset_size = std::min(target_subset_size, num_partitions);
        hot_partitions.resize(actual_subset_size);
        // We create a list of all partition IDs and shuffle it to get a random subset
        std::vector<std::uint32_t> all_parts(num_partitions);
        std::iota(all_parts.begin(), all_parts.end(), 0);

        // Shuffling with a deterministic RNG to ensure reproducibility
        std::mt19937_64 rng(seed + 0xDEADBEEF); 
        std::shuffle(all_parts.begin(), all_parts.end(), rng);

        // We take the first 'actual_subset_size' partition IDs as the hot subset
        for(std::uint32_t i = 0; i < actual_subset_size; ++i) {
            hot_partitions[i] = all_parts[i];
        }
    }
    // =================================================================
    // RECORD GENERATION
    // =================================================================
    for (std::size_t i = 0; i < n; ++i) {
        // If max_key is zero, we can skip all the generation logic and just set keys to zero
        if (max_key == 0) {
            out[i].key = 0ULL;
            continue;
        }

        // 1. We first decide whether to generate a skewed key or a uniform key based on the skew_factor
        std::uint64_t r_skew = splitmix64_next(state);
        double u = static_cast<double>(r_skew) / static_cast<double>(-1ULL);

        // 2. If u < skew_factor, we generate a skewed key; otherwise, we generate a uniform key
        if (u < skew_factor && target_subset_size > 0) {
            // =================================================================
            // SKEWED PATH (Sintesi Deterministica)
            // =================================================================
            
            // 1. We extract a uniform value between 0.0 and 1.0 for the partition selection
            std::uint64_t r_part = splitmix64_next(state);
            double u_idx = static_cast<double>(r_part) / static_cast<double>(-1ULL);

            // 2. We apply a distortion (Power-law) to create the crest effect
            double skewed_u = std::pow(u_idx, crest_shape);

            // 3. We calculate the index squeezed towards zero
            std::uint32_t idx = static_cast<std::uint32_t>(skewed_u * hot_partitions.size());

            // We ensure that the index does not go out of bounds due to rounding
            if (idx >= hot_partitions.size()) {
                idx = hot_partitions.size() - 1;
            }
            
            // 4. We select the target partition ID from the hot subset
            std::uint32_t p_target = hot_partitions[idx];

            if (max_key < (1ULL << 32)) {
                // -------------------------------------------------------------
                // CASE A: K_max < 2^32 (No XOR Cancellation)
                // -------------------------------------------------------------
                std::uint64_t max_multiples = max_key / num_partitions;

                if (max_multiples == 0) {
                    // If max_multiples is zero, it means max_key < num_partitions, so we can just take p_target mod max_key
                    out[i].key = p_target % max_key;
                } else {
                    // R % max_multiples generates a value in [0, (K_max/P) - 1]
                    // equivalent to U(0, floor(K_max/P) - 1) and avoids negative underflow
                    std::uint64_t r_mult = splitmix64_next(state);
                    std::uint64_t R = r_mult % max_multiples; 
                    // The final key is constructed to ensure it maps to the target partition
                    out[i].key = (R * num_partitions) + p_target;
                }
            } 
            else {
                // -------------------------------------------------------------
                // CASE B: K_max >= 2^32 (Hierarchical Bounding & Cancellation)
                // -------------------------------------------------------------
                
                // 1. Upper Half Generation (Assorbe il vincolo max_key)
                std::uint64_t k_high_bound = max_key >> 32;
                std::uint64_t r_high = splitmix64_next(state);
                std::uint64_t k_high = r_high % k_high_bound; // Strictly < K_max >> 32

                // 2. Noise Extraction (O_high = k_high mod P)
                std::uint64_t o_high = k_high % num_partitions;

                // 3. Padding Generation (R strictly confined to 32-bit space)
                std::uint64_t max_multiples_32 = (1ULL << 32) / num_partitions;
                std::uint64_t r_mult = splitmix64_next(state);
                std::uint64_t R = r_mult % max_multiples_32; // Ensures that k_low will never overflow into the upper 32 bits

                // 4. Pre-emptive Cancellation (Anti-venom) & Low 32-bit Synthesis
                std::uint64_t B = p_target ^ o_high;
                std::uint64_t k_low = (R * num_partitions) + B;

                // 5. 64-bit Assembly
                out[i].key = (k_high << 32) | k_low;
            }
        } 
        else {
            // =================================================================
            // UNIFORM PATH (Fallback logica originale)
            // =================================================================
            const std::uint64_t r = splitmix64_next(state);
            out[i].key = r % max_key;
        }
    }
    return out;
}


// ------------------------------------------------------------
// Intentionally simple partition mapping
// ------------------------------------------------------------
//
// This mapping is deliberately minimal.
// It is here only so that the reference code is complete and runnable.
//
// Students must replace this function with their own implementation from Module 1.
// The same mapping function must be used consistently in both the sequential
// and parallel versions to ensure a fair performance comparison.
//
// If P is a power of two, then key & (P-1) maps into [0, P).
// This is fast, but intentionally simplistic.
//
static inline std::uint16_t compute_partition_id(std::uint64_t key, std::uint32_t p) {
    std::uint32_t mask = p - 1U;
    std::uint64_t mixed = key ^ (key >> 32);
    return static_cast<std::uint16_t>(mixed & mask);
}
// ------------------------------------------------------------
// Histogram
// ------------------------------------------------------------
//
// Count how many records go to each partition.
//
// hist[pid] = number of records whose key maps to pid
//
static std::vector<std::vector<std::size_t>> compute_histogram_matrix(
    const std::vector<Record>& rel, std::uint32_t p, std::uint32_t num_threads) 
{
    // local histograms: each thread has its own private histogram to avoid contention 
    // local_hists[thread_id][pid] = count of records for partition pid in thread thread_id
    // This 2D structure allows us to compute histograms in parallel without synchronization,
    // and then we can aggregate them later.
    std::vector<std::vector<std::size_t>> local_hists(num_threads, std::vector<std::size_t>(p, 0));

    #pragma omp parallel default(none) \
        shared(local_hists, rel, p) \
        num_threads(num_threads)
    {
        int tid = omp_get_thread_num();
        auto& my_hist = local_hists[tid];

        #pragma omp for schedule(static)
        for (std::size_t i = 0; i < rel.size(); ++i) {
            std::uint32_t pid = compute_partition_id(rel[i].key, p);
            my_hist[pid]++; 
        }
    }
    // Aggregate local histograms into the final histogram
    // This step is done sequentially after all threads have completed their local histograms.
    // We iterate over each partition and sum the counts from all threads for that partition.
    // This is a reduction phase where we combine the results from all threads into a single histogram.
    // The final result is a 2D histogram matrix where each row corresponds to a thread
    return local_hists;
}




// ------------------------------------------------------------
// Prefix sum (exclusive scan)
// ------------------------------------------------------------
//
// Given a histogram, compute the begin offsets of each partition.
//
// Example:
//   hist  = [0, 1, 2, 5]
//   begin = [0, 0, 1, 3]
//
// Then partition p occupies [begin[p], begin[p] + hist[p]).
//
struct PrefixSumResult {
    std::vector<std::size_t> begin;                         // begin[p] = global starting offset of partition p in the output array
    std::vector<std::vector<std::size_t>> thread_offsets;   // thread_offsets[t][p] = starting offset for thread t to write records of partition p during scatter
};


// This function computes the exclusive prefix sum for a 2D histogram matrix.
// It calculates the global starting offsets for each partition and also computes the individual offsets for each thread to use during the scatter phase.

static PrefixSumResult exclusive_prefix_sum_2d(
    const std::vector<std::vector<std::size_t>>& local_hists, std::uint32_t p) 
{
    // local_hists is a 2D vector where local_hists[t][pid] gives the count of records for partition pid in thread t.
    // We need to compute:
    // 1. begin[pid]: the global starting offset of partition pid in the output array.
    // 2. thread_offsets[t][pid]: the starting offset for thread t to write records of partition pid during the scatter phase.
    std::uint32_t num_threads = local_hists.size();
    PrefixSumResult res;
    res.begin.assign(p, 0);
    res.thread_offsets.assign(num_threads, std::vector<std::size_t>(p, 0));

    std::size_t running_offset = 0;

    // Compute the global starting offsets for each partition
    // We iterate over each partition and calculate the total count of records for that partition across all threads.
    // The global starting offset for partition pid is the sum of the counts of all previous partitions.
    for (std::uint32_t pid = 0; pid < p; ++pid) {
        res.begin[pid] = running_offset;  // Set the global starting offset for partition pid
        
        // For each thread, we assign a starting offset for partition pid.
        for (std::uint32_t t = 0; t < num_threads; ++t) {
            // The thread offset for thread t and partition pid is the current running offset, which is the global starting offset plus the counts of this partition from all previous threads.
            res.thread_offsets[t][pid] = running_offset;
            // After assigning the offset for thread t, we need to update the running offset by adding the count of records for partition pid in thread t, so that the next thread gets the correct offset.
            running_offset += local_hists[t][pid];
        }
    }

    // After this loop, res.begin[pid] contains the global starting offset for partition pid, and res.thread_offsets[t][pid] contains the starting offset for thread t to write records of partition pid during the scatter phase.
    return res;
}

// ------------------------------------------------------------
// Scatter into a partitioned array
// ------------------------------------------------------------
//
// Reorder records so that all records belonging to the same partition become
// contiguous in memory.
//
// We use a write cursor per partition, initialized from the begin offsets.
//
// This is a parallel scatter implementation that uses local buffers to minimize contention on the output array.
// Each thread maintains a local buffer for each partition, and flushes it to the output array when it is full. This reduces the number of atomic operations and cache line contention on the output array.
// The buffer size is chosen to fit within a cache line (e.g., 64 bytes) to optimize memory access patterns. 

constexpr std::size_t BUFFER_SIZE = 64 ; 

static std::vector<Record> scatter_omp_parallel(
    const std::vector<Record>& rel,
    std::uint32_t p,
    std::uint32_t num_threads,
    const std::vector<std::vector<std::size_t>>& thread_offsets)
{
    // Output array for scattered records
    std::vector<Record> out(rel.size());
    
    // Parallel scatter using OpenMP
    #pragma omp parallel num_threads(num_threads) \
        default(none) shared(rel, out, p, thread_offsets)
    {
        // Each thread maintains a local buffer for each partition to reduce contention on the output array.
        int tid = omp_get_thread_num();
        std::vector<Record> local_buffer(p * BUFFER_SIZE);
        std::vector<std::size_t> buffer_counts(p, 0);
        std::vector<std::size_t> my_next = thread_offsets[tid];

        // Each thread processes a portion of the input relation and scatters records into the output array using its local buffer. 
        // When a local buffer for a partition is full, it is flushed to the output array, and the corresponding write cursor is updated.
        #pragma omp for schedule(static)
        for (std::size_t i = 0; i < rel.size(); ++i) {
            const std::uint32_t pid = compute_partition_id(rel[i].key, p);
            local_buffer[pid * BUFFER_SIZE + buffer_counts[pid]] = rel[i];

            if (++buffer_counts[pid] == BUFFER_SIZE) {
                std::memcpy(&out[my_next[pid]], &local_buffer[pid * BUFFER_SIZE], BUFFER_SIZE * sizeof(Record));
                my_next[pid] += BUFFER_SIZE;
                buffer_counts[pid] = 0;
            }
        }

        // Flush finale
        for (std::uint32_t pid = 0; pid < p; ++pid) {
            if (buffer_counts[pid] > 0) {
                std::memcpy(&out[my_next[pid]], &local_buffer[pid * BUFFER_SIZE], buffer_counts[pid] * sizeof(Record));
            }
        }
    }
    // After all threads have completed, the output array 'out' contains the scattered records, where records belonging to the same partition are stored contiguously. We can return this array as the result of the scatter phase.
    return out;
}


// ------------------------------------------------------------
// Partitioned relation metadata
// ------------------------------------------------------------
//
// This stores:
//   - the partitioned array
//   - the begin offset of each partition
//   - the end offset of each partition
//
// So partition p is data[begin[p] .. end[p]).
//
struct PartitionedRelation {
    std::vector<Record> data;
    std::vector<std::size_t> begin;
    std::vector<std::size_t> end;
};

// ------------------------------------------------------------
// Full partitioning pipeline for one relation
// ------------------------------------------------------------
//
// This groups together the steps:
//   histogram -> prefix sum -> scatter -> end offsets
//
// After this phase, all records belonging to the same partition
// are stored contiguously in memory, enabling independent processing.
//
static PartitionedRelation partition_relation(const std::vector<Record>& rel, std::uint32_t p, std::uint32_t num_threads, PhaseTiming::PartitionPhase& T_part) {
    std::vector<std::vector<std::size_t>> local_hists;
    PrefixSumResult prefix_res;
    std::vector<std::size_t> end(p);
    std::vector<Record> data;

    {
        ScopedTimer t(T_part.histogram);
        local_hists = compute_histogram_matrix(rel, p, num_threads);
    }

    {
        ScopedTimer t(T_part.prefix);
        prefix_res = exclusive_prefix_sum_2d(local_hists, p);
    }

    {
        ScopedTimer t(T_part.scatter);
        data = scatter_omp_parallel(rel, p, num_threads, prefix_res.thread_offsets);
    }

    {
        ScopedTimer t(T_part.end);
        for (std::uint32_t i = 0; i < p - 1; i++) {
            end[i] = prefix_res.begin[i + 1];
        }
        end[p - 1] = rel.size(); 
    }

    return PartitionedRelation{
        .data = std::move(data),
        .begin = prefix_res.begin,
        .end = end
    };
}

// ------------------------------------------------------------
// Join result
// ------------------------------------------------------------
struct JoinResult {
    std::uint64_t join_count = 0;
    std::uint64_t checksum1 = 0;
    std::uint64_t checksum2 = 0;
};

// ------------------------------------------------------------
// Local join on one partition
// ------------------------------------------------------------
//
// We process one partition p as follows:
//
//   Build:
//     Scan R_p and compute countR[key]
//
//   Probe:
//     Scan S_p
//     If key k is present in countR, then each occurrence in S_p matches
//     countR[k] occurrences in R_p.
//
// Duplicates are handled by counting occurrences in R.
// Each record in S contributes as many matches as the multiplicity
// of its key in the corresponding partition of R.
//
static JoinResult join_one_partition_optimized(const PartitionedRelation& Rpart,
                                     const PartitionedRelation& Spart,
                                     std::uint32_t pid,
                                     PhaseTiming& T/*,
                                     ska::flat_hash_map<std::uint64_t, std::uint32_t>& countR*/) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];
    const std::size_t s_begin = Spart.begin[pid];
    const std::size_t s_end = Spart.end[pid];

    // Nothing to do if either partition is empty.
    if (r_begin == r_end || s_begin == s_end) {
        return result;
    }


    ska::flat_hash_map<std::uint64_t, std::uint32_t> countR;
    countR.reserve((r_end - r_begin) * 2);

    // Build phase:
    {
        ScopedTimer t(T.build);
        for (std::size_t i = r_begin; i < r_end; ++i) {
            ++countR[Rpart.data[i].key];
        }
    }


    // Probe phase:
    // for each key in S_p, if it exists in countR, add countR[key] matches.
    {
        ScopedTimer t(T.probe);
        for (std::size_t i = s_begin; i < s_end; ++i) {
            const std::uint64_t key = Spart.data[i].key;
            const auto it = countR.find(key);
            if (it != countR.end()) {
                const std::uint64_t multiplicity = it->second;

                result.join_count += multiplicity;
                result.checksum1 += splitmix64(key) * multiplicity;
                result.checksum2 += splitmix64(key ^ 0x9e3779b97f4a7c15ULL) * multiplicity;
            }
        }
    }

    return result;
}

// ------------------------------------------------------------
// Full sequential partitioned hash join
// ------------------------------------------------------------
//
// This is the end-to-end baseline:
//
//   1. Partition R
//   2. Partition S
//   3. For each partition p:
//        local build + local probe
//   4. Accumulate results
//
// Each partition can be processed independently.
// This property is the basis for parallelization in Module 2.
//

// Strutture allineate per forzare ogni elemento su una Cache Line indipendente (64 bytes)
struct alignas(64) AlignedJoinResult {
    JoinResult data;
};

struct alignas(64) AlignedPhaseTiming {
    PhaseTiming data;
};

static JoinResult partitioned_hash_join(
    const std::vector<Record>& R,
    const std::vector<Record>& S,
    std::uint32_t p,
    std::uint32_t num_threads,
    PhaseTiming& T)
{
    JoinResult total{};

    // -------------------------
    // Phase 1: partitioning R
    // -------------------------
    PartitionedRelation Rpart;
    {
        ScopedTimer t(T.R.total);
        Rpart = partition_relation(R, p, num_threads, T.R);
    }

    // -------------------------
    // Phase 2: partitioning S
    // -------------------------
    PartitionedRelation Spart;
    {
        ScopedTimer t(T.S.total);
        Spart = partition_relation(S, p, num_threads, T.S);
    }

    // -------------------------
    // Phase 3: join phase
    // ------------------------
    std::vector<AlignedJoinResult> thread_results(num_threads);
    std::vector<AlignedPhaseTiming> thread_timings(num_threads);

    {
        ScopedTimer t(T.join_loop); 

        #pragma omp parallel num_threads(num_threads) \
            default(none) shared(Rpart, Spart, T, R, thread_results, thread_timings, p, std::cout)       

        {
            // Each thread has a unique ID in [0, num_threads), which it can obtain using omp_get_thread_num(). This ID is used to index into the thread_results and thread_timings arrays, ensuring that each thread writes its results to a separate cache line, thus avoiding false sharing and contention.
            int threadid = omp_get_thread_num();

            // each thread maintains its own local totals for join count and checksums, as well as timing information for the build and probe phases. This allows threads to accumulate their results independently without contention, and then we can aggregate these results at the end of the parallel region.
            JoinResult local_total{};
            PhaseTiming local_timing{}; 


            // We parallelize the loop over partitions using OpenMP. Each thread will process a subset of the partitions, and since each partition is independent, there
            #pragma omp for schedule(runtime) \
                nowait // nowait allows threads to proceed to the next iteration without waiting for others to finish, which can improve load balancing when partitions have varying sizes.
            for (std::uint32_t pid = 0; pid < p; ++pid) {
                
                
                JoinResult local = join_one_partition_optimized(Rpart, Spart, pid, local_timing);
                
                // After processing its assigned partitions, each thread updates its local totals for join count and checksums by adding the results from the current partition to the local_total. This accumulation is done independently by each thread, so there
                local_total.join_count += local.join_count;
                local_total.checksum1  += local.checksum1;
                local_total.checksum2  += local.checksum2;

            }

            // After all threads have completed their assigned partitions, they write their local totals and timing information into the thread_results and thread_timings arrays at the index corresponding to their thread ID. This allows us to aggregate the results later without any contention, as each thread writes to a separate cache line.
            thread_results[threadid].data = local_total;
            thread_timings[threadid].data = local_timing;
        } 
        // After the parallel region, we have the results from all threads stored in thread_results and thread_timings. We can then aggregate these results to compute the final join count, checksums, and timing information for the entire join operation.
    }

    // Aggregate results from all threads
    for (std::uint32_t id = 0; id < num_threads; ++id) {
        total.join_count += thread_results[id].data.join_count;
        total.checksum1  += thread_results[id].data.checksum1;
        total.checksum2  += thread_results[id].data.checksum2;
        
        T.build = std::max(T.build, thread_timings[id].data.build);
        T.probe = std::max(T.probe, thread_timings[id].data.probe);
    }

    return total;
}
// ------------------------------------------------------------
// Verifier for very small inputs
// ------------------------------------------------------------
//
// This is useful only for debugging and correctness testing on tiny
// datasets. It checks all pairs directly, so its complexity is O(|R|*|S|).
//
static JoinResult naive_join_verifier(const std::vector<Record>& R,
                                      const std::vector<Record>& S) {
    JoinResult result{};

    for (const auto& r : R) {
        for (const auto& s : S) {
            if (r.key == s.key) {
                result.join_count += 1;
                result.checksum1 += splitmix64(r.key);
                result.checksum2 += splitmix64(r.key ^ 0x9e3779b97f4a7c15ULL);
            }
        }
    }
    return result;
}


// ------------------------------------------------------------
// Full sequential partitioned hash join (for correctness checking)
// ------------------------------------------------------------
// Histogram
// ------------------------------------------------------------
//
// Count how many records go to each partition.
//
// hist[pid] = number of records whose key maps to pid
//
static std::vector<std::size_t> compute_histogram(const std::vector<Record>& rel, std::uint32_t p) {
    std::vector<std::size_t> hist(p, 0);

    for (const auto& rec : rel) {
        const std::uint32_t pid = compute_partition_id(rec.key, p);
        ++hist[pid];
    }
    
    return hist;
}

// ------------------------------------------------------------
// Prefix sum (exclusive scan)
// ------------------------------------------------------------
//
// Given a histogram, compute the begin offsets of each partition.
//
// Example:
//   hist  = [0, 1, 2, 5]
//   begin = [0, 0, 1, 3]
//
// Then partition p occupies [begin[p], begin[p] + hist[p]).
//
static std::vector<std::size_t> exclusive_prefix_sum(const std::vector<std::size_t>& hist) {
    std::vector<std::size_t> begin(hist.size(), 0);

    std::size_t running = 0;
    for (std::size_t p = 0; p < hist.size(); ++p) {
        begin[p] = running;
        running += hist[p];
    }
    return begin;
}

// ------------------------------------------------------------
// Scatter into a partitioned array
// ------------------------------------------------------------
//
// Reorder records so that all records belonging to the same partition become
// contiguous in memory.
//
// We use a write cursor per partition, initialized from the begin offsets.
//
static std::vector<Record> scatter_partitioned(const std::vector<Record>& rel,
                                               std::uint32_t p,
                                               const std::vector<std::size_t>& begin) {
    std::vector<Record> out(rel.size());

    // Current write position for each partition.
    std::vector<std::size_t> next = begin;

    for (const auto& rec : rel) {
        const std::uint32_t pid = compute_partition_id(rec.key, p);
        out[next[pid]++] = rec;
    }

    return out;
}


// ------------------------------------------------------------
// Full partitioning pipeline for one relation
// ------------------------------------------------------------
//
// This groups together the steps:
//   histogram -> prefix sum -> scatter -> end offsets
//
// After this phase, all records belonging to the same partition
// are stored contiguously in memory, enabling independent processing.
//
static PartitionedRelation partition_relation(const std::vector<Record>& rel, std::uint32_t p, PhaseTiming::PartitionPhase& T_part) {
    std::vector<std::size_t> hist;
    std::vector<std::size_t> begin;
    std::vector<Record> data;
    std::vector<std::size_t> end(p);

    {
        ScopedTimer t(T_part.histogram);
        hist = compute_histogram(rel, p);
    }

    {
        ScopedTimer t(T_part.prefix);
        begin = exclusive_prefix_sum(hist);
    }

    {
        ScopedTimer t(T_part.scatter);
        data = scatter_partitioned(rel, p, begin);
    }

    {
        ScopedTimer t(T_part.end);
        for (std::uint32_t i = 0; i < p; i++) {
            end[i] = begin[i] + hist[i];
        }
    }

    return PartitionedRelation{
        .data = std::move(data),
        .begin = begin,
        .end = end
    };
}



// ------------------------------------------------------------
// Local join on one partition
// ------------------------------------------------------------
//
// We process one partition p as follows:
//
//   Build:
//     Scan R_p and compute countR[key]
//
//   Probe:
//     Scan S_p
//     If key k is present in countR, then each occurrence in S_p matches
//     countR[k] occurrences in R_p.
//
// Duplicates are handled by counting occurrences in R.
// Each record in S contributes as many matches as the multiplicity
// of its key in the corresponding partition of R.
//
static JoinResult join_one_partition(const PartitionedRelation& Rpart,
                                     const PartitionedRelation& Spart,
                                     std::uint32_t pid,
                                     PhaseTiming& T) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];
    const std::size_t s_begin = Spart.begin[pid];
    const std::size_t s_end = Spart.end[pid];

    // Nothing to do if either partition is empty.
    if (r_begin == r_end || s_begin == s_end) {
        return result;
    }

    // Build phase:
    // count how many times each key appears in R_p.
	//
    ska::flat_hash_map<std::uint64_t, std::uint32_t> countR;
    countR.reserve((r_end - r_begin) * 2);

    {
        ScopedTimer t(T.build);
        for (std::size_t i = r_begin; i < r_end; ++i) {
            ++countR[Rpart.data[i].key];
        }
    }


    // Probe phase:
    // for each key in S_p, if it exists in countR, add countR[key] matches.
    {
        ScopedTimer t(T.probe);
        for (std::size_t i = s_begin; i < s_end; ++i) {
            const std::uint64_t key = Spart.data[i].key;
            const auto it = countR.find(key);
            if (it != countR.end()) {
                const std::uint64_t multiplicity = it->second;

                result.join_count += multiplicity;
                result.checksum1 += splitmix64(key) * multiplicity;
                result.checksum2 += splitmix64(key ^ 0x9e3779b97f4a7c15ULL) * multiplicity;
            }
        }
    }

    return result;
}

// ------------------------------------------------------------
// Full sequential partitioned hash join
// ------------------------------------------------------------
//
// This is the end-to-end baseline:
//
//   1. Partition R
//   2. Partition S
//   3. For each partition p:
//        local build + local probe
//   4. Accumulate results
//
// Each partition can be processed independently.
// This property is the basis for parallelization in Module 2.
//
static JoinResult partitioned_hash_join_sequential(
    const std::vector<Record>& R,
    const std::vector<Record>& S,
    std::uint32_t p,
    PhaseTiming& T)
{
    JoinResult total{};

    // -------------------------
    // Phase 1: partitioning R
    // -------------------------
    PartitionedRelation Rpart;
    {
        ScopedTimer t(T.R.total);
        Rpart = partition_relation(R, p, T.R);
    }

    // -------------------------
    // Phase 2: partitioning S
    // -------------------------
    PartitionedRelation Spart;
    {
        ScopedTimer t(T.S.total);
        Spart = partition_relation(S, p, T.S);
    }

    // -------------------------
    // Phase 3: join phase
    // -------------------------
    {
        ScopedTimer t(T.join_loop);

        for (std::uint32_t pid = 0; pid < p; ++pid) {
            JoinResult local = join_one_partition(Rpart, Spart, pid, T);

            total.join_count += local.join_count;
            total.checksum1  += local.checksum1;
            total.checksum2  += local.checksum2;
        }
    }

    return total;
}




OmpSchedule parse_schedule(const std::string& s)
{
    if (s == "static")  return OmpSchedule::STATIC;
    if (s == "dynamic") return OmpSchedule::DYNAMIC;
    if (s == "guided")  return OmpSchedule::GUIDED;
    return OmpSchedule::AUTO;
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0, chunk_size = 1,subset_size = 0;
    double sigma = 0.0, crest_shape = 1.0;
    std::string sched = "";
    int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;


    if (!read_arg_u64(argc, argv, "-nr", nr) ||
        !read_arg_u64(argc, argv, "-ns", ns) ||
        !read_arg_u64(argc, argv, "-seed", seed) ||
        !read_arg_u64(argc, argv, "-max-key", max_key) ||
        !read_arg_u64(argc, argv, "-p", p)||
        !read_arg_double(argc, argv, "-sigma", sigma) ||
        !read_arg_u64(argc, argv, "-subset-size", subset_size) ||
        !read_arg_double(argc, argv, "-crest-shape", crest_shape) ||
        !read_arg_string(argc, argv, "-sched", sched)
    ) {
        usage(argv[0]);
        return 1;
    }
    std::uint64_t t_input = 0;
    if (read_arg_u64(argc, argv, "-t", t_input)) {
        nthreads = static_cast<int>(t_input);
    }
    
    read_arg_u64(argc, argv, "-chunk", chunk_size);

    if (nthreads <= 0) {
        std::cerr << "Error: Number of threads must be at least 1.\n";
        return 1;
    }

    const std::uint32_t P = static_cast<std::uint32_t>(p);

    OmpSchedule omp_sched = parse_schedule(sched);

    // The power-of-two constraint on P is only due to the default
	// mapping used here. It may be removed if the chosen partition
	// function correctly handles arbitrary P
    if (!is_power_of_two(P)) {
        std::cerr << "Error: P must be power of two.\n";
        return 1;
    }

    const std::size_t NR = static_cast<std::size_t>(nr);
    const std::size_t NS = static_cast<std::size_t>(ns);

    // Deterministic generation.
    // We use two different seeds so that R and S are not identical.
    const auto R = generate_relation(NR, seed, max_key, P, sigma, subset_size, crest_shape);
    const auto S = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, sigma, subset_size, crest_shape);

    const int RUNS = 10;

    std::vector<PhaseTiming> times;
    std::vector<JoinResult> results;
    times.reserve(RUNS);
    results.reserve(RUNS);

    // Set the OpenMP schedule for the join loop. This will affect how partitions are distributed among threads.
    set_schedule(omp_sched, chunk_size);


    for (int i = 0; i < RUNS + 2; i++) {
        PhaseTiming T;

        const auto t0 = std::chrono::steady_clock::now();
        JoinResult r = partitioned_hash_join(R, S, P, nthreads, T);
        const auto t1 = std::chrono::steady_clock::now();

        T.total = std::chrono::duration<double>(t1 - t0).count();

        // We discard the first few runs to allow for warm-up and more stable measurements.
        if (i > 2)
        {
            times.push_back(T);
            results.push_back(r);
        }
    }

    // Utility to extract timing data for statistics printing.
    auto extract = [&](auto getter) {
        std::vector<double> v;
        v.reserve(times.size());
        for (const auto& t : times) {
            v.push_back(getter(t));
        }
        return v;
    };

    const JoinResult& result = results.back();


    std::cout << "NR=" << NR << " NS=" << NS << " P=" << P
              << " seed=" << seed
              << " [0, " << max_key << ")\n";

    std::cout << "join_count=" << result.join_count << "\n";
    std::cout << "checksum1=" << result.checksum1 << "\n";
    std::cout << "checksum2=" << result.checksum2 << "\n";

    std::cout << std::fixed << std::setprecision(6);

    if (NR <= 500 && NS <= 500) {
        const JoinResult naive = naive_join_verifier(R, S);
        std::cout << "naive_join_count=" << naive.join_count << "\n";
        std::cout << "naive_checksum1=" << naive.checksum1 << "\n";
        std::cout << "naive_checksum2=" << naive.checksum2 << "\n";
    }

    std::cout << "\n========== Sequential version (check for correcteness) ==========\n";
    PhaseTiming T;
    JoinResult r = partitioned_hash_join_sequential(R, S, P, T);
    std::cout << "join_count=" << r.join_count << "\n";
    std::cout << "checksum1=" << r.checksum1 << "\n";
    std::cout << "checksum2=" << r.checksum2 << "\n";
    if(NR <= 500 && NS <= 500) {
        const JoinResult naive = naive_join_verifier(R, S);
        std::cout << "naive_join_count=" << naive.join_count << "\n";
        std::cout << "naive_checksum1=" << naive.checksum1 << "\n";
        std::cout << "naive_checksum2=" << naive.checksum2 << "\n";
    }

    std::cout << "\n========== BENCHMARK PROFILE ==========\n";

    print_group("PARTITION R");
    print_stats("  histogram", extract([](const PhaseTiming& t){ return t.R.histogram; }));
    print_stats("  prefix",    extract([](const PhaseTiming& t){ return t.R.prefix; }));
    print_stats("  scatter",   extract([](const PhaseTiming& t){ return t.R.scatter; }));
    print_stats("  end",       extract([](const PhaseTiming& t){ return t.R.end; }));
    print_stats("  total_R",   extract([](const PhaseTiming& t){ return t.R.total; }));

    print_group("PARTITION S");
    print_stats("  histogram", extract([](const PhaseTiming& t){ return t.S.histogram; }));
    print_stats("  prefix",    extract([](const PhaseTiming& t){ return t.S.prefix; }));
    print_stats("  scatter",   extract([](const PhaseTiming& t){ return t.S.scatter; }));
    print_stats("  end",       extract([](const PhaseTiming& t){ return t.S.end; }));
    print_stats("  total_S",   extract([](const PhaseTiming& t){ return t.S.total; }));

    print_group("JOIN PHASE");
    print_stats("  build",     extract([](const PhaseTiming& t){ return t.build; }));
    print_stats("  probe",     extract([](const PhaseTiming& t){ return t.probe; }));
    print_stats("  join_loop", extract([](const PhaseTiming& t){ return t.join_loop; }));

    print_group("GLOBAL");
    print_stats("TOTAL", extract([](const PhaseTiming& t){ return t.total; }));

    return 0;
}