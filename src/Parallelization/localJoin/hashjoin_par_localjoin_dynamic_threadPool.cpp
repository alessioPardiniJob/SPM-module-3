// hashjoin_par_localjoin_dynamic_threadPool.cpp
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
#include <thread>
#include "utils/utils.hpp"
#include <cassert>
#include "threadPool.hpp" 
// ------------------------------------------------------------
// Record definition
// ------------------------------------------------------------
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
        << "  " << prog << " -nr NR -ns NS -seed SEED -max-key K -p P [-chunk C]\n\n"
        << "Parameters:\n"
        << "  -nr         Number of records in relation R\n"
        << "  -ns         Number of records in relation S\n"
        << "  -seed       Deterministic seed\n"
        << "  -max-key    Keys are generated in [0, max-key)\n"
        << "  -p          Number of partitions (power of two required in this reference code)\n"
        << "  -chunk      Number of partitions per task (default 1)\n";
}
static bool is_power_of_two(std::uint32_t x) {
    return x != 0 && (x & (x - 1U)) == 0;
}

// ------------------------------------------------------------
// Deterministic pseudo-random generation
// ------------------------------------------------------------
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
static inline std::uint16_t compute_partition_id(std::uint64_t key, std::uint32_t p) {
    std::uint32_t mask = p - 1U;
    std::uint64_t mixed = key ^ (key >> 32);
    return static_cast<std::uint16_t>(mixed & mask);
}

// ------------------------------------------------------------
// Histogram
// ------------------------------------------------------------
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
static std::vector<Record> scatter_partitioned(const std::vector<Record>& rel,
                                               std::uint32_t p,
                                               const std::vector<std::size_t>& begin) {
    std::vector<Record> out(rel.size());
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
struct PartitionedRelation {
    std::vector<Record> data;
    std::vector<std::size_t> begin;
    std::vector<std::size_t> end;
};

// ------------------------------------------------------------
// Full partitioning pipeline for one relation
// ------------------------------------------------------------
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
static JoinResult join_one_partition(const PartitionedRelation& Rpart,
                                     const PartitionedRelation& Spart,
                                     std::uint32_t pid,
                                     PhaseTiming& T) {
    JoinResult result{};

    const std::size_t r_begin = Rpart.begin[pid];
    const std::size_t r_end = Rpart.end[pid];
    const std::size_t s_begin = Spart.begin[pid];
    const std::size_t s_end = Spart.end[pid];

    if (r_begin == r_end || s_begin == s_end) {
        return result;
    }

    ska::flat_hash_map<std::uint64_t, std::uint32_t> countR;
    countR.reserve((r_end - r_begin) * 2);

    {
        ScopedTimer t(T.build);
        for (std::size_t i = r_begin; i < r_end; ++i) {
            ++countR[Rpart.data[i].key];
        }
    }

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
// Dynamic partitioned hash join (Threadpool)
// ------------------------------------------------------------
struct alignas(64) AlignedJoinResult {
    JoinResult data;
};

struct alignas(64) AlignedPhaseTiming {
    PhaseTiming data;
};

static JoinResult partitioned_hash_join_dynamic(
    const std::vector<Record>& R,
    const std::vector<Record>& S,
    std::uint32_t p,
    std::uint32_t nthreads,
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
    // Phase 3: join phase (Dynamic scheduling via Threadpool)
    // ------------------------
    
    std::uint32_t num_tasks = (p + chunk_size - 1) / chunk_size;
    std::vector<AlignedJoinResult> task_results(num_tasks);
    std::vector<AlignedPhaseTiming> thread_timings(nthreads);

    {
        ScopedTimer t(T.join_loop); 

        threadPool TP(nthreads); 
   
        // Task function for each chunk of partitions
        auto taskF = [&](std::uint32_t task_id) {
            int tid = TP.thread_id();
            assert(tid >= 0 && tid < 32);

            std::uint32_t lower = task_id * chunk_size;
            std::uint32_t upper = std::min(lower + chunk_size, p);

            // local accumulators for this task
            JoinResult local_total{};
            PhaseTiming local_timing{}; 

            // Process assigned partitions
            for (std::uint32_t pid = lower; pid < upper; ++pid) {
                JoinResult local = join_one_partition(
                    Rpart, Spart, pid, local_timing
                );                
                local_total.join_count += local.join_count;
                local_total.checksum1  += local.checksum1;
                local_total.checksum2  += local.checksum2;
            }
            
            // Store results in the task-specific array
            task_results[task_id].data = local_total;
            thread_timings[tid].data.build += local_timing.build;
            thread_timings[tid].data.probe += local_timing.probe;
        };

        // Submit tasks to the thread pool for dynamic scheduling
        for (std::uint32_t task_id = 0; task_id < num_tasks; ++task_id) {
            TP.submit(taskF, task_id);
        }

        // Wait for all tasks to complete
        TP.wait_all(); 
    }
    
    // Accumulate results from tasks
    for (std::uint32_t id = 0; id < num_tasks; ++id) {
        total.join_count += task_results[id].data.join_count;
        total.checksum1  += task_results[id].data.checksum1;
        total.checksum2  += task_results[id].data.checksum2;
    }

    // Extract timing from threads (critical path)
    for (std::uint32_t id = 0; id < nthreads; ++id) {
        T.build = std::max(T.build, thread_timings[id].data.build);
        T.probe = std::max(T.probe, thread_timings[id].data.probe);
    }

    return total;
}

// ------------------------------------------------------------
// Verifier for very small inputs
// ------------------------------------------------------------
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

    if (!read_arg_u64(argc, argv, "-nr", nr) ||
        !read_arg_u64(argc, argv, "-ns", ns) ||
        !read_arg_u64(argc, argv, "-seed", seed) ||
        !read_arg_u64(argc, argv, "-max-key", max_key) ||
        !read_arg_u64(argc, argv, "-p", p)) {
        usage(argv[0]);
        return 1;
    }

    read_arg_u64(argc, argv, "-chunk", chunk_size);

    if (p > std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "Error: P too large.\n";
        return 1;
    }

    const std::uint32_t P = static_cast<std::uint32_t>(p);

    if (!is_power_of_two(P)) {
        std::cerr << "Error: P must be power of two.\n";
        return 1;
    }

    const std::size_t NR = static_cast<std::size_t>(nr);
    const std::size_t NS = static_cast<std::size_t>(ns);

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
        JoinResult r = partitioned_hash_join_dynamic(R, S, P, nthreads, chunk_size, T);
        const auto t1 = std::chrono::steady_clock::now();

        T.total = std::chrono::duration<double>(t1 - t0).count();

        // We discard the first few runs to allow for warm-up and more stable measurements.
        if (i > 2)
        {
            times.push_back(T);
            results.push_back(r);
        }
    }

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

    print_group("JOIN PHASE (DYNAMIC POOL)");
    print_stats("  build",     extract([](const PhaseTiming& t){ return t.build; }));
    print_stats("  probe",     extract([](const PhaseTiming& t){ return t.probe; }));
    print_stats("  join_loop", extract([](const PhaseTiming& t){ return t.join_loop; }));

    print_group("GLOBAL");
    print_stats("TOTAL", extract([](const PhaseTiming& t){ return t.total; }));

    return 0;
}