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

static bool read_arg_u32(int argc, char** argv, const std::string& name, std::uint32_t& out) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (name == argv[i]) {
            out = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
            return true;
        }
    }
    return false;
}



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
// Utility: Usage message
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
//// =========================================================
// PHASE 1: Task-based Histogram
// =========================================================
// Sostituiamo gli offset legati al thread con offset legati al singolo "chunk" (Task)
struct PrefixSumResult {
    std::vector<std::size_t> begin;                     
    std::vector<std::vector<std::size_t>> chunk_offsets; 
};
// ------------------------------------------------------------
// Join result
// ------------------------------------------------------------
struct JoinResult {
    std::uint64_t join_count = 0;
    std::uint64_t checksum1 = 0;
    std::uint64_t checksum2 = 0;
};
// aligned struct for thread-local results to avoid false sharing
struct alignas(64) ThreadState {
    JoinResult result{};
    PhaseTiming timing{};
    ska::flat_hash_map<std::uint64_t, std::uint32_t> map;
};

// =========================================================
// PHASE 1: Task-based Histogram
// =========================================================
static std::vector<std::vector<std::size_t>> compute_histogram_tasks(
    const std::vector<Record>& rel, std::uint32_t p, std::uint32_t num_chunks) 
{
    // Matrix [num_chunks][p]
    std::vector<std::vector<std::size_t>> chunk_hists(num_chunks, std::vector<std::size_t>(p, 0));
    std::size_t chunk_size = (rel.size() + num_chunks - 1) / num_chunks;

    // We use OpenMP tasks to compute the histogram in parallel. Each task will process a chunk of the input relation and update its local histogram for that chunk. This approach allows us to achieve better load balancing, especially when the distribution of keys is skewed, as tasks can be dynamically scheduled by the OpenMP runtime.
    #pragma omp parallel default(none) shared(rel, chunk_hists, p, num_chunks, chunk_size)
    {
        #pragma omp single 
        {
            // The single directive ensures that only one thread (the master thread) will execute the following block, which is responsible for creating the tasks. The tasks themselves can be executed by any thread in the team, allowing for dynamic load balancing.
            #pragma omp task untied default(none) shared(rel, chunk_hists, p, num_chunks, chunk_size)
            {
                // We create 'num_chunks' tasks, each responsible for computing the histogram for a specific chunk of the input relation. The chunk size is calculated to ensure that all records are covered, even if the total number of records is not perfectly divisible by the number of chunks.
                for (std::uint32_t c = 0; c < num_chunks; ++c) {
                    
                    // For each chunk, we create a task that will compute the histogram for that chunk. The task is marked as untied to allow it to be executed by any thread, which can help with load balancing. Each task will compute the histogram for its assigned chunk and update the corresponding row in the chunk_hists matrix.
                    #pragma omp task default(none) firstprivate(c, chunk_size) shared(rel, chunk_hists, p, num_chunks)
                    {
                        // Each task computes the histogram for its assigned chunk. We calculate the start and end indices for the chunk, ensuring that we do not go out of bounds. The task then iterates over the records in its chunk, computes the partition ID for each record, and updates the local histogram for that chunk accordingly.
                        std::size_t start = c * chunk_size;
                        std::size_t end = std::min(start + chunk_size, rel.size());
                        
                        // We compute the histogram for the current chunk. Each thread updates its local histogram for the chunk it is processing, which avoids contention on shared data structures. The partition ID is computed using the compute_partition_id function, and the corresponding count in the chunk_hists matrix is incremented.
                        for (std::size_t i = start; i < end; ++i) {
                            std::uint32_t pid = compute_partition_id(rel[i].key, p);
                            chunk_hists[c][pid]++;
                        }
                    }
                }
            } // master thread creates all tasks, then implicitly waits for them to complete at the end of the single block. Each task updates its local histogram for its assigned chunk, and once all tasks are done, we have the complete histogram in chunk_hists.
        } // end of single block, implicit barrier here ensures all tasks are completed before we proceed
    }
    
    // After the parallel region, we have the complete histogram for all chunks stored in chunk_hists. Each row corresponds to a chunk, and each column corresponds to a partition, with the value representing the count of records in that chunk that map to that partition. This histogram will be used in the next phase to compute the prefix sums and determine the offsets for scattering the records into their respective partitions.
    return chunk_hists;
}

// =========================================================
// PHASE 2: Prefix Sum 
// =========================================================
static PrefixSumResult exclusive_prefix_sum_2d_tasks(
    const std::vector<std::vector<std::size_t>>& chunk_hists, std::uint32_t p) 
{

    // We compute the exclusive prefix sum across the chunk histograms to determine the starting offsets for each partition in the output array. The begin vector will store the global starting offset for each partition, while the chunk_offsets matrix will store the starting offset for each partition within each chunk. This allows us to efficiently scatter records into their correct positions in the output array during the next phase.
    std::uint32_t num_chunks = chunk_hists.size();
    PrefixSumResult res;
    res.begin.assign(p, 0);
    res.chunk_offsets.assign(num_chunks, std::vector<std::size_t>(p, 0));

    std::size_t running_offset = 0;

    // We iterate over each partition and compute the exclusive prefix sum for that partition across all chunks. The begin vector is updated with the global starting offset for each partition, while the chunk_offsets matrix is updated with the starting offset for each partition within each chunk. The running_offset variable is used to keep track of the cumulative count of records as we compute the prefix sums.
    for (std::uint32_t pid = 0; pid < p; ++pid) {
        res.begin[pid] = running_offset; 
        // We compute the exclusive prefix sum for the current partition across all chunks. For each chunk, we set the starting offset for that partition in the chunk_offsets matrix to the current running_offset, and then we increment the running_offset by the count of records in that chunk for the current partition. This ensures that we have the correct offsets for scattering records into their respective partitions in the next phase.
        for (std::uint32_t c = 0; c < num_chunks; ++c) {
            res.chunk_offsets[c][pid] = running_offset;
            running_offset += chunk_hists[c][pid];
        }
    }

    // After this loop, the res.begin vector contains the global starting offsets for each partition, and the res.chunk_offsets matrix contains the starting offsets for each partition within each chunk. This information will be crucial for efficiently scattering records into their correct positions in the output array during the next phase.
    return res;
}

// =========================================================
// PHASE 3: Task-based Scatter
// =========================================================
constexpr std::size_t BUFFER_SIZE = 64;

static std::vector<Record> scatter_omp_tasks(
    const std::vector<Record>& rel,
    std::uint32_t p,
    std::uint32_t num_chunks,
    const std::vector<std::vector<std::size_t>>& chunk_offsets)
{
    std::vector<Record> out(rel.size());
    std::size_t chunk_size = (rel.size() + num_chunks - 1) / num_chunks;

    int max_threads = omp_get_max_threads();
    // Pre-allocazione sicura
    std::vector<std::vector<Record>> thread_buffers(max_threads, std::vector<Record>(p * BUFFER_SIZE));
    std::vector<std::vector<std::size_t>> thread_counts(max_threads, std::vector<std::size_t>(p, 0));

    #pragma omp parallel default(none) shared(rel, out, p, chunk_offsets, num_chunks, chunk_size, thread_buffers, thread_counts)
    {
        // 1. MACRO-TASK GENERATORE (Untied per evitare il bottleneck)
        #pragma omp single
        {
            #pragma omp task untied shared(rel, out, p, chunk_offsets, num_chunks, chunk_size, thread_buffers, thread_counts)
            {
                for (std::uint32_t c = 0; c < num_chunks; ++c) {
                    
                    // 2. TASK WORKER (Tied è meglio qui per la sicurezza del buffer index tid)
                    #pragma omp task firstprivate(c, chunk_size) shared(rel, out, p, chunk_offsets, num_chunks, thread_buffers, thread_counts)
                    {
                        int tid = omp_get_thread_num();
                        auto& local_buffer = thread_buffers[tid];
                        auto& buffer_counts = thread_counts[tid];
                        
                        std::fill(buffer_counts.begin(), buffer_counts.end(), 0);
                        
                        std::vector<std::size_t> my_next = chunk_offsets[c];

                        std::size_t start = c * chunk_size;
                        std::size_t end = std::min(start + chunk_size, rel.size());

                        for (std::size_t i = start; i < end; ++i) {
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
                }
            }
        }
    }
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
//// =========================================================
// Interfaccia del Partizionamento unificata
// =========================================================
static PartitionedRelation partition_relation_tasks(const std::vector<Record>& rel, 
                                                    std::uint32_t p, 
                                                    std::uint32_t num_threads, 
                                                    PhaseTiming::PartitionPhase& T_part) {
    // Definizione del parallelismo di task: oversubscription
    // Creiamo più chunk (task) rispetto ai thread per garantire il bilanciamento del carico
    std::uint32_t num_chunks = num_threads;
    
    std::vector<std::vector<std::size_t>> chunk_hists;
    PrefixSumResult prefix_res;
    std::vector<std::size_t> end(p);
    std::vector<Record> data;

    {
        ScopedTimer t(T_part.histogram);
        chunk_hists = compute_histogram_tasks(rel, p, num_chunks);
    }
    {
        ScopedTimer t(T_part.prefix);
        prefix_res = exclusive_prefix_sum_2d_tasks(chunk_hists, p);
    }
    {
        ScopedTimer t(T_part.scatter);
        data = scatter_omp_tasks(rel, p, num_chunks, prefix_res.chunk_offsets);
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
                                     PhaseTiming& T,
                                     ska::flat_hash_map<std::uint64_t, std::uint32_t>& countR) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];
    const std::size_t s_begin = Spart.begin[pid];
    const std::size_t s_end = Spart.end[pid];

    // Nothing to do if either partition is empty.
    if (r_begin == r_end || s_begin == s_end) {
        return result;
    }


    std::size_t current_r_size = r_end - r_begin;
    std::size_t needed_buckets = current_r_size * 2; // load factor ~0.5

    if (countR.bucket_count() > needed_buckets ) {
        // the map is too big, we swap it with an empty one to free memory. This is O(1) and avoids the O(n) cost of clear() on a large map.
        ska::flat_hash_map<std::uint64_t, std::uint32_t> fresh;
        fresh.reserve(current_r_size);
        countR.swap(fresh);
    } else {
        // the map is not too big, we can just clear it. clear() is O(n) but if the map is not much larger than needed, this might be more efficient than swapping with a fresh map.
        countR.clear();
        // reserve the needed capacity to avoid rehashing during insertions. This is O(1) if the current capacity is already sufficient, otherwise it will trigger a rehash which is O(n). However, since we are reserving for the exact size we need, this should minimize the chance of multiple rehashes.
        countR.reserve(current_r_size);
    }

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

struct alignas(64) AlignedJoinResult {
    JoinResult data;
};

struct alignas(64) AlignedPhaseTiming {
    PhaseTiming data;
};

static JoinResult partitioned_hash_join_tasks(
    const std::vector<Record>& R,
    const std::vector<Record>& S,
    std::uint32_t p,
    std::uint32_t num_threads,
    std::uint32_t chunk_size,
    PhaseTiming& T)
{
    JoinResult total{};

    PartitionedRelation Rpart;
    {
        ScopedTimer t(T.R.total);
        Rpart = partition_relation_tasks(R, p, num_threads, T.R);
    }

    PartitionedRelation Spart;
    {
        ScopedTimer t(T.S.total);
        Spart = partition_relation_tasks(S, p, num_threads, T.S);
    }

    // Vectors for storing final results and timings from each thread to avoid false sharing. Each thread will write its result to a separate cache line, and we will aggregate them at the end.
    std::vector<AlignedJoinResult> final_results(num_threads);
    std::vector<AlignedPhaseTiming> final_timings(num_threads);

    // array of pointers to thread-local states, indexed by thread ID. Each thread will write to its own state, and we will aggregate the results at the end. This allows us to avoid false sharing while still having a convenient way to access each thread's local state.
    std::vector<ThreadState*> state_ptrs(num_threads, nullptr);

    {
        ScopedTimer t(T.join_loop);

        #pragma omp parallel num_threads(num_threads) default(none) \
        shared(Rpart, Spart, p, num_threads, R, chunk_size, state_ptrs, final_results, final_timings) 
        {
            int tid = omp_get_thread_num();

            // thread-local state for the join results and timing, as well as the hash map for the build phase. Each thread will have its own instance of ThreadState, which includes a JoinResult for accumulating the results of the join and a PhaseTiming for measuring the time spent in the build and probe phases. The hash map is used for counting occurrences of keys in the build phase and is reserved to minimize rehashing.
            ThreadState local_state;
            local_state.map.reserve((R.size() / p) * 2);

            // We store a pointer to the local state in the shared state_ptrs vector at the index corresponding to the thread ID. This allows other threads (or tasks) to access this thread's local state when needed, while still ensuring that each thread writes to its own cache line to avoid false sharing.
            state_ptrs[tid] = &local_state;

            // We use OpenMP tasks to process the partitions in parallel. The master thread creates tasks for processing chunks of partitions, and each task can be executed by any thread in the team. This allows for dynamic load balancing, especially in cases where the distribution of keys across partitions is skewed. Each task will process a range of partition IDs, and within each task, we will compute the join results for those partitions and accumulate them in the thread-local state.
            #pragma omp barrier // Ensure all threads have initialized their local state and stored their pointers before any thread starts creating tasks that might access those pointers.

            // The single directive ensures that only one thread (the master thread) will execute the following block, which is responsible for creating the tasks for processing the partitions. Each task will be responsible for processing a chunk of partition IDs, and the tasks can be executed by any thread in the team, allowing for dynamic load balancing.
            #pragma omp single
            {
                #pragma omp task untied default(none) \
                shared(Rpart, Spart, p, num_threads, chunk_size, state_ptrs)
                {
                    // We create tasks for processing the partitions in chunks. Each task will process a range of partition IDs, and within each task, we will compute the join results for those partitions and accumulate them in the thread-local state. The chunk size can be tuned to balance the overhead of task creation with the granularity of parallelism.
                    for (std::uint32_t start_pid = 0; start_pid < p; start_pid += chunk_size) {
                        std::uint32_t end_pid = std::min(start_pid + chunk_size, p);
                        
                        // For each chunk of partition IDs, we create a task that will compute the join results for those partitions. The task is marked as untied to allow it to be executed by any thread, which can help with load balancing. Each task will access the thread-local state of the executing thread through the state_ptrs vector, and it will accumulate the join results for its assigned partitions into that state.
                        #pragma omp task default(none) firstprivate(start_pid, end_pid) \
                        shared(Rpart, Spart, state_ptrs)
                        {
                            int exec_tid = omp_get_thread_num();
                            ThreadState& my_state = *(state_ptrs[exec_tid]);

                            JoinResult task_local_total{};
                            PhaseTiming task_local_timing{};

                            // We compute the join results for the current chunk of partitions.
                            for (std::uint32_t pid = start_pid; pid < end_pid; ++pid) {
                                JoinResult local = join_one_partition_optimized(
                                    Rpart, Spart, pid, task_local_timing, my_state.map);
                                
                                // We accumulate the results for this partition into the task-local total. This allows us to minimize the number of updates to the thread-local state, which can help reduce contention and improve cache performance. After processing all partitions in the chunk, we will have a total join result for that chunk, which we can then add to the thread-local state in one go.
                                task_local_total.join_count += local.join_count;
                                task_local_total.checksum1  += local.checksum1;
                                task_local_total.checksum2  += local.checksum2;
                            }
                            // After processing the assigned chunk of partitions, we add the task-local totals to the thread-local state. This is done in one step to minimize contention on the thread-local state and to ensure that each thread updates its own state without interference from other threads.
                            my_state.result.join_count += task_local_total.join_count;
                            my_state.result.checksum1  += task_local_total.checksum1;
                            my_state.result.checksum2  += task_local_total.checksum2;
                            // We also accumulate the timing information for the build and probe phases into the thread-local state. This allows us to measure the time spent in these phases for each thread, which can be useful for performance analysis and tuning.
                            my_state.timing.build += task_local_timing.build;
                            my_state.timing.probe += task_local_timing.probe;
                        }
                    }
                }
            } // end of single block, implicit barrier here ensures all tasks are completed before we proceed to aggregate results

            // Copying the results from the thread-local state to the final results vector. Each thread writes its result to a separate index in the final_results vector, which is aligned to avoid false sharing. This allows us to safely aggregate the results at the end without contention between threads.
            final_results[tid].data = local_state.result;
            final_timings[tid].data = local_state.timing;

        } // destroying the thread-local states as we exit the parallel region. Each thread will have written its results to the final_results and final_timings vectors, so we can safely aggregate those results after this point without needing to access any thread-local state.
    } 

    // Aggregate results from all threads. We sum up the join counts and checksums, and we take the maximum build and probe times across all threads to get the total time spent in those phases. This gives us the final join result and timing information for the entire partitioned hash join operation.
    for (std::uint32_t id = 0; id < num_threads; ++id) {
        total.join_count += final_results[id].data.join_count;
        total.checksum1  += final_results[id].data.checksum1;
        total.checksum2  += final_results[id].data.checksum2;
        
        T.build = std::max(T.build, final_timings[id].data.build);
        T.probe = std::max(T.probe, final_timings[id].data.probe);
    }

    // After this aggregation, the total variable contains the final join result, and the T variable contains the timing information for the build and probe phases. We can then return the total join result to the caller.
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
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0;
    std::uint32_t subset_size = 0, chunk_size = 1;
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
        !read_arg_u32(argc, argv, "-subset-size", subset_size) ||
        !read_arg_double(argc, argv, "-crest-shape", crest_shape) ||
        !read_arg_u32(argc, argv, "-chunk-size", chunk_size)
    ) {
        usage(argv[0]);
        return 1;
    }
    std::uint64_t t_input = 0;
    if (read_arg_u64(argc, argv, "-t", t_input)) {
        nthreads = static_cast<int>(t_input);
    }
    

    if (nthreads <= 0) {
        std::cerr << "Error: Number of threads must be at least 1.\n";
        return 1;
    }

    const std::uint32_t P = static_cast<std::uint32_t>(p);


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



    for (int i = 0; i < RUNS + 2; i++) {
        PhaseTiming T;

        const auto t0 = std::chrono::steady_clock::now();
        JoinResult r = partitioned_hash_join_tasks(R, S, P, nthreads, chunk_size, T);
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

    print_group("TEARDOWN");
    print_stats("  teardown",  extract([](const PhaseTiming& t){ return t.teardown; }));

    print_group("GLOBAL");
    print_stats("TOTAL", extract([](const PhaseTiming& t){ return t.total; }));

    return 0;
}