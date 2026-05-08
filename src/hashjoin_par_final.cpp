// ------------------------------------------------------------
// Output:
//   join_count
//   checksum1
//   checksum2
//
//
// The code follows these phases:
//
//   1. Input generation
//      Generate two relations R and S with deterministic keys.
//
//   2. Partitioning of R and S
//      The goal of this phase is to reorganize the data so that
//      records belonging to the same partition are stored contiguously.
//
//      This is done in three steps:
//
//      - mapping key -> partition id
//        Each key is mapped to a partition identifier in [0, P).
//
//      - histogram
//        Count how many records are assigned to each partition.
//        This tells us how much space each partition will occupy.
//
//      - prefix sum (offset computation)
//        Convert counts into starting positions (offsets) for each partition
//        in the output array.
//
//      - scatter
//        Move each record to its correct position so that all records
//        of the same partition are stored contiguously.
//
//      After this phase, each partition corresponds to a contiguous
//      segment of the array, and can be processed independently.
//
//   3. Local join per partition
//      For each partition p:
//
//      - build
//        Scan the R partition and count how many times each key appears.
//
//      - probe
//        Scan the corresponding S partition.
//        For each key, if it exists in R, add as many matches as its multiplicity.
//
//   4. Final output
//      Accumulate results across all partitions.
//
// The result does NOT materialize the join pairs.
// It only computes:
//   - total number of matches
//   - two checksums for correctness verification
//
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

#include <vector>
#include <thread>
#include <cstring>
#include <algorithm>
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
static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " -nr NR -ns NS -seed SEED -max-key K -p P\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n";
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


static std::vector<Record> generate_relation(std::size_t n, std::uint64_t seed, std::uint64_t max_key) {
    std::vector<Record> out(n);
    std::uint64_t state = seed;

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t r = splitmix64_next(state);
        out[i].key = (max_key == 0) ? 0ULL : (r % max_key);
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

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    // block-based partitioning: each thread processes a contiguous block of records
    auto block_based = [&](int threadid) {
        // Each thread computes its own local histogram for its assigned block of records.
        std::size_t total_records = rel.size();
        std::size_t parts_per_thread = total_records / num_threads;
        std::size_t rem = total_records % num_threads;
        
        // Compute the range of records this thread will process
        std::size_t lower = threadid * parts_per_thread + std::min(static_cast<std::size_t>(threadid), rem);
        std::size_t upper = lower + parts_per_thread + (threadid < static_cast<int>(rem) ? 1 : 0);

        // Compute local histogram for the assigned block of records
        auto& my_hist = local_hists[threadid];
        for (std::size_t i = lower; i < upper; ++i) {
            std::uint32_t pid = compute_partition_id(rel[i].key, p);
            my_hist[pid]++; 
        }
    };

    // Launch threads to compute local histograms in parallel
    for (std::uint32_t id = 0; id < num_threads; ++id) {
        threads.emplace_back([&, threadid = id]() { block_based(threadid); });
    }
    // Wait for all threads to finish
    for (auto& t : threads) { t.join(); }

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
constexpr std::size_t BUFFER_SIZE = 8; 

static std::vector<Record> scatter_parallel_optimized(
    const std::vector<Record>& rel,
    std::uint32_t p,
    std::uint32_t num_threads,
    const std::vector<std::vector<std::size_t>>& thread_offsets)
{
    // Output array for scattered records
    std::vector<Record> out(rel.size());
    
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    // Worker function for each thread to scatter records into the output array
    auto worker = [&, num_threads](std::uint32_t threadid)
    {
        // ---------------------------
        // 1. LOCAL BUFFERS
        // ---------------------------
        std::vector<Record> local_buffer(p * BUFFER_SIZE);
        std::vector<std::size_t> buffer_counts(p, 0);
        
        // my_next[pid] will be the next write position for thread threadid to write records of partition pid.
        std::vector<std::size_t> my_next = thread_offsets[threadid];

        // ---------------------------
        // 2. COMPUTE RANGE
        // ---------------------------
        const std::size_t total = rel.size();
        const std::size_t base = total / num_threads;
        const std::size_t rem  = total % num_threads;

        const std::size_t tid = threadid;
        const std::size_t lower = tid * base + std::min<std::size_t>(tid, rem);
        const std::size_t upper = lower + base + (tid < rem ? 1 : 0);

        // ---------------------------
        // 3. CORE LOOP
        // ---------------------------
        for (std::size_t i = lower; i < upper; ++i)
        {
            const std::uint32_t pid = compute_partition_id(rel[i].key, p);

            // Write the record to the local buffer for this partition
            auto& buf = local_buffer[pid * BUFFER_SIZE + buffer_counts[pid]];
            buf = rel[i];

            // Increment the buffer count for this partition. If the buffer is full, flush it to the output array.
            if (++buffer_counts[pid] == BUFFER_SIZE)
            {
                std::memcpy(
                    &out[my_next[pid]],
                    &local_buffer[pid * BUFFER_SIZE],
                    BUFFER_SIZE * sizeof(Record)
                );

                my_next[pid] += BUFFER_SIZE;
                buffer_counts[pid] = 0;
            }
        }

        // ---------------------------
        // 4. FLUSH FINALE
        // ---------------------------
        // After processing all records, we need to flush any remaining records in the local buffers to the output array.
        for (std::uint32_t pid = 0; pid < p; ++pid)
        {
            if (buffer_counts[pid] > 0)
            {
                std::memcpy(
                    &out[my_next[pid]],
                    &local_buffer[pid * BUFFER_SIZE],
                    buffer_counts[pid] * sizeof(Record)
                );
            }
        }
    };

    // ---------------------------
    // 5. SPAWN & JOIN THREADS
    // ---------------------------
    for (std::uint32_t id = 0; id < num_threads; ++id)
    {
        threads.emplace_back(worker, id);
    }

    // Wait for all threads to finish
    for (auto& t : threads)
    {
        t.join();
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
        data = scatter_parallel_optimized(rel, p, num_threads, prefix_res.thread_offsets);
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


    countR.clear();
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

// Strutture allineate per forzare ogni elemento su una Cache Line indipendente (64 bytes)
struct alignas(64) AlignedJoinResult {
    JoinResult data;
};

struct alignas(64) AlignedPhaseTiming {
    PhaseTiming data;
};

static JoinResult partitioned_hash_join_sequential(
    const std::vector<Record>& R,
    const std::vector<Record>& S,
    std::uint32_t p,
    std::uint32_t num_threads,
    std::uint32_t chunk_size,
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
    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    {
        ScopedTimer t(T.join_loop); 

        // Block-cyclic distribution strategy
        auto block_cyclic = [&](int threadid) {
            std::uint32_t offset = threadid * chunk_size;
            std::uint32_t stride = num_threads * chunk_size;

            // local accumulators for this thread
            JoinResult local_total{};
            PhaseTiming local_timing{}; 


            ska::flat_hash_map<std::uint64_t, std::uint32_t> thread_map;
            // Reserve space in the hash map to avoid dynamic resizing during the join phase, which can be costly.
            thread_map.reserve((R.size() / p) * 2);

            for (std::uint32_t lower = offset; lower < p; lower += stride) {
                std::uint32_t upper = std::min(lower + chunk_size, p);
                for (std::uint32_t pid = lower; pid < upper; ++pid) {
                    // Process partition pid and accumulate results in local_total and local_timing
                    JoinResult local = join_one_partition_optimized(Rpart, Spart, pid, local_timing, thread_map);
                    
                    // Accumulate results in local_total to minimize the number of writes to the shared thread_results array, which can cause contention.
                    local_total.join_count += local.join_count;
                    local_total.checksum1  += local.checksum1;
                    local_total.checksum2  += local.checksum2;
                }
            }
            
            // Store results in the thread-specific array
            // Writing the final results to the shared thread_results array only once per thread helps to reduce contention
            thread_results[threadid].data = local_total;
            thread_timings[threadid].data = local_timing;
        };

        // Spawn threads
        for (std::uint32_t id = 0; id < num_threads; ++id) {
            threads.emplace_back([&, threadid = id]() {
                block_cyclic(threadid);
            });
        }

        // Wait for all threads to finish
        for (auto& thread : threads) {
            thread.join();
        }
    }
    
    // Accumulate results and timings from all threads accedendo tramite .data
    for (std::uint32_t id = 0; id < num_threads; ++id) {
        total.join_count += thread_results[id].data.join_count;
        total.checksum1  += thread_results[id].data.checksum1;
        total.checksum2  += thread_results[id].data.checksum2;
        
        // Take the max time among threads to represent the critical path / bottleneck
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
// Main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0, chunk_size = 1;
    int nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 1;


    if (!read_arg_u64(argc, argv, "-nr", nr) ||
        !read_arg_u64(argc, argv, "-ns", ns) ||
        !read_arg_u64(argc, argv, "-seed", seed) ||
        !read_arg_u64(argc, argv, "-max-key", max_key) ||
        !read_arg_u64(argc, argv, "-p", p)) {
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
    const auto R = generate_relation(NR, seed, max_key);
    const auto S = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key);

    const int RUNS = 10;

    std::vector<PhaseTiming> times;
    std::vector<JoinResult> results;
    times.reserve(RUNS);
    results.reserve(RUNS);

    for (int i = 0; i < RUNS + 2; i++) {
        PhaseTiming T;

        const auto t0 = std::chrono::steady_clock::now();
        JoinResult r = partitioned_hash_join_sequential(R, S, P, nthreads, chunk_size, T);
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