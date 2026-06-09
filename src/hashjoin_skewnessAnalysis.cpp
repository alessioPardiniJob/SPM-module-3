
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
#include <numeric> 
#include <iomanip> 
#include <fstream> 
#include <unordered_map>

#include <vector>
#include <numeric>
#include <algorithm>
#include <random>
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
// Utility: usage message
// ------------------------------------------------------------
static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " -nr NR -ns NS -seed SEED -max-key K -p P\n\n"
        << "Parameters:\n"
        << "  -nr          Number of records in relation R\n"
        << "  -ns          Number of records in relation S\n"
        << "  -seed        Deterministic seed\n"
        << "  -max-key     Keys are generated in [0, max-key)\n"
        << "  -p           Number of partitions (power of two required in this reference code)\n"
        << "  -sigma       Skew factor (0.0 = totalmente uniforme)\n"
        << "  -subset-size Target subset size for skewed data\n"
        << "  -crest-shape  Shape of the skew crest (1.0 = plateau uniforme, >1.0 = cresta appuntita verso l'indice 0)\n";
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



static inline std::uint16_t compute_partition_id(std::uint64_t key, std::uint32_t p) {
    std::uint32_t mask = p - 1U;
    std::uint64_t mixed = key ^ (key >> 32);
    return static_cast<std::uint16_t>(mixed & mask);
}

void print_histogram(const std::vector<std::size_t>& hist, double skew_factor ) {
    std::size_t total_records = std::accumulate(hist.begin(), hist.end(), 0ULL);

    std::ofstream csv_file;

    // We generate two different CSV files for uniform and skewed data to facilitate LaTeX plotting
    if (skew_factor == 0.0) {
        // Uniform data
        csv_file.open("uniform_data.csv");
        if (!csv_file.is_open()) {
            std::cerr << "Errore: Impossibile creare il file uniform_data.csv!\n";
        } else {
            csv_file << "id,records\n";
        }
    } else {
        // Skewed data
        csv_file.open("skew_data.csv");
        if (!csv_file.is_open()) {
            std::cerr << "Errore: Impossibile creare il file skew_data.csv!\n";
        } else {
            csv_file << "id,records\n";
        }
    }

    std::cout << "\n======================================================\n";
    std::cout << "   DISTRIBUZIONE DEL CARICO SULLE PARTIZIONI\n";
    std::cout << "   (Record Totali Generati: " << total_records << ")\n";
    std::cout << "======================================================\n";
    
    std::cout << std::left << std::setw(15) << "Partition ID" 
              << std::setw(20) << "Numero Record" 
              << "Percentuale\n";
    std::cout << "------------------------------------------------------\n";

    // We print the histogram data to the console and also write it to a CSV file for LaTeX plotting
    for (std::size_t pid = 0; pid < hist.size(); ++pid) {
        double percentage = (total_records == 0) ? 0.0 : 
            (static_cast<double>(hist[pid]) / total_records) * 100.0;
        
        std::cout << std::left << std::setw(15) << pid 
                  << std::setw(20) << hist[pid] 
                  << std::fixed << std::setprecision(2) << percentage << " %\n";

        if (csv_file.is_open()) {
            csv_file << pid << "," << hist[pid] << "\n";
        }
    }
    std::cout << "======================================================\n";
    
    if (csv_file.is_open()) {
        csv_file.close();
        std::cout << " -> File 'skew_data.csv' generato con successo per LaTeX!\n";
    }
    std::cout << "\n";
}




void analyze_dataset_and_generate_csv(const std::vector<Record>& rel, std::size_t num_partitions, double skew_factor = 0.0) {
    
    // Count occurrences of each key to analyze skewness at the key level
    std::unordered_map<std::uint64_t, std::size_t> key_counts;
    
    // Count how many records go to each partition to analyze skewness at the partition level
    std::vector<std::size_t> hist(num_partitions, 0);

    // We iterate through the relation once to populate both the key counts and the partition histogram
    for (const auto& record : rel) {
        key_counts[record.key]++;
        std::size_t pid = compute_partition_id(record.key, num_partitions);
        if (pid < num_partitions) {
            hist[pid]++;
        }
    }
    // We can also compute some statistics about the key distribution if needed (e.g., number of unique keys, max frequency, etc.)
    print_histogram(hist, skew_factor);
}




// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char** argv) {
    std::uint64_t nr = 0, ns = 0, seed = 0, max_key = 0, p = 0, subset_size = 0;
    double sigma = 0.0, crest_shape = 1.0;

    if (!read_arg_u64(argc, argv, "-nr", nr) ||
        !read_arg_u64(argc, argv, "-ns", ns) ||
        !read_arg_u64(argc, argv, "-seed", seed) ||
        !read_arg_u64(argc, argv, "-max-key", max_key) ||
        !read_arg_u64(argc, argv, "-p", p) ||
        !read_arg_double(argc, argv, "-sigma", sigma) ||
        !read_arg_double(argc, argv, "-crest-shape", crest_shape) ||
        !read_arg_u64(argc, argv, "-subset-size", subset_size)) {
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
    const auto R = generate_relation(NR, seed, max_key, P, sigma, subset_size, crest_shape);
    const auto S = generate_relation(NS, seed ^ 0xdeadebdecdeedef1ULL, max_key, P, sigma, subset_size, crest_shape);



    // Analyze the generated dataset and produce CSV files
    analyze_dataset_and_generate_csv(R, P, sigma);
}