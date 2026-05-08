// hashjoin_seq.cpp
//
// Sequential reference for Module 2
// Partitioned Hash Join with Duplicates
//
// This code is intentionally written to be simple and readable.
// It is meant as a reference baseline and as a starting point for
// the parallel version. You can modify it for improving performance,
// provided you do not change the overall algorithm.
// 
//
// IMPORTANT:
// The function compute_partition_id(...) below is intentionally very simple.
// Students must replace it with their own mapping function from Module 1.
// The same mapping function must be used consistently in both the sequential
// and parallel versions.
//
// Run example:
//   ./hashjoin_seq -nr 5 -ns 8 -seed 13 -max-key 8 -p 4
//
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
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cmath>
#include "utils/utils.hpp"
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
	// NOTE: Adopting std::unordered_map is an implementation choice
	// of the reference code, not a mandatory part of the algorithm itself.
	// Students may discuss its impact on performance and, if properly justified,
	// replace it with alternative structures in their analysis or optimized versions,
	// provided that the overall join logic remains unchanged
	//
    std::unordered_map<std::uint64_t, std::uint32_t> countR;
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
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0;

    if (!read_arg_u64(argc, argv, "-nr", nr) ||
        !read_arg_u64(argc, argv, "-ns", ns) ||
        !read_arg_u64(argc, argv, "-seed", seed) ||
        !read_arg_u64(argc, argv, "-max-key", max_key) ||
        !read_arg_u64(argc, argv, "-p", p)) {
        usage(argv[0]);
        return 1;
    }

    if (p > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Error: P too large.\n";
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
        JoinResult r = partitioned_hash_join_sequential(R, S, P, T);
        const auto t1 = std::chrono::steady_clock::now();

        T.total = std::chrono::duration<double>(t1 - t0).count();

        // We discard the first few runs to allow for warm-up and more stable measurements.
        if (i > 2)
        {
            times.push_back(T);
            results.push_back(r);
        }

    }

    // Nuova lambda per poter estrarre valori anche da sottostrutture (es. t.R.histogram)
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