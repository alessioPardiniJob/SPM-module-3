# High-Performance Hash Join Optimization

## Introduction
This repository contains a comprehensive study and implementation of a high-performance Parallel Hash Join algorithm. 
Through a series of incremental experiments, we explore:
1. **Data Structure Efficiency**: Moving from node-based maps to hardware-conscious open-addressing maps.
2. **Parallel Scheduling**: Evaluating Static (Block-based/Cyclic) vs. Dynamic (Thread Pool) strategies.
4. **Scalability**: Analyzing performance behavior under Strong and Weak scaling scenarios on multi-core architectures.

---

## Repository Structure
```text
.
├── bin/                                # Compiled executables
├── src/                                # Source code directory
│   ├── hashjoin_par_final.cpp          # Final optimized parallel version
│   ├── hashjoin_seq_flat_map.cpp       # Sequential version using ska::flat_hash_map
│   ├── hashjoin_seq_unordered_map.cpp  # Sequential version using std::unordered_map
│   └── Parallelization/
│       ├── localJoin/                  # Experiment 1: Local Join strategies
│       │   ├── ..._dynamic_threadPool.cpp
│       │   ├── ..._static_blockBased.cpp
│       │   ├── ..._static_blockCyclic.cpp
│       │   └── ..._static_blockCyclic_Optimized.cpp
│       └── partitioning/               # Experiment 2: Partitioning strategies
│           ├── compute_histogram/      # Atomic vs. Local histogram computation
│           └── scatter/                # Atomic vs. Buffered vs. Local scatter
├── strong_scalability.sh               # Script for strong scalability testing
├── third_party/                        # External libraries (ska::flat_hash_map)
├── utils/                              # Core helpers (ThreadPool, Affinitization, Timers)
│   ├── affinity.hpp                    # Thread affinity helpers
│   ├── hpc_helpers.hpp                 # High-performance helpers
│   ├── taskFactory.hpp                 # Task factory helpers
│   ├── threadPool.hpp                  # Thread pool helpers
│   └── utils.hpp                       # General helpers
├── tune_chunk_*.sh                     # Scripts for tuning parallel parameters
├── Makefile                            # Build system rules
└── weak_scalability.sh                 # Script for weak scalability testing
└── strong_scalability.sh               # Script for strong scalability testing

```


# EXPERIMENT 1 — Local Join Parallelization Strategy
This section explores how to efficiently distribute the workload of the local join phase across threads. Since the relations are partitioned, join operations for each partition are entirely independent, allowing for lock-free parallelization.
## HOW TO COMPILE
```bash 
make parallel_localjoin_static_blockbased
make parallel_localjoin_static_blockcyclic
make parallel_localjoin_dynamic_threadpool
make parallel_localjoin_static_blockcyclic_optimized
```
## EXPERIMENT 1.1 - Tuning of Chunk Size
Determines the optimal configuration for chunk sizes across the Block-Cyclic and Dynamic strategies.
### HOW TO EXECUTE
```bash 
./tune_chucnk_dynamic_threadPool.sh
```
```bash 
./tune_chucnk_static_blockCyclic.sh
```
Output to check: Look at the median execution times across the different chunk sizes (e.g., C=1 through C=64) to identify the optimal parameter for workload distribution. [log_chunk_tuning_... .txt files]
## EXPERIMENT 1.2 - Workload Characterization & Scheduling
Compares three scheduling strategies to handle workload balance: Static Block-Based, Static Block-Cyclic, and Dynamic Thread-Pool.
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_localjoin_dynamic_threadPool -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256 -chunk 1
```
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_localjoin_static_blockBased -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
```
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_localjoin_static_blockCyclic -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256 -chunk 16
```
Output to check: Compare the final join_loop times. In a uniform data setup, Block-Cyclic typically performs best (e.g., ~211 ms) due to the naturally balanced distribution. 

## EXPERIMENT 1.3 - Memory Allocation Hoisting Optimization
Addresses the overhead of repeatedly constructing and destroying hash maps across all partitions by allocating a single map locally per thread outside the main loop.
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_localjoin_static_blockCyclic_Optimized -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256 -chunk 16
```
Output to check: Look for the reduction in the join_loop execution time compared to Experiment 1.2.

## EXPERIMENT 2 — Partitioning Parallelization Strategy
This phase tackles the memory-bandwidth-bound operations of partitioning: calculating the histograms and scattering the data.
## HOW TO COMPILE
```bash 
make parallel_partitioning_compute_histogram_globalatomic
make parallel_partitioning_compute_histogram_local
make parallel_partitioning_scatter_globalatomic
make parallel_partitioning_scatter
make parallel_partitioning_scatter_buffered
```
## EXPERIMENT 2.1 - Compute histogram
Compares a naive approach using shared atomic updates against a thread-local privatization strategy. 
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_compute_histogram_globalAtomic -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_compute_histogram_local -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
Output to check: Notice the severe bottleneck in the global atomic run due to memory bus contention. The local privatization version should be faster.
## EXPERIMENT 2.2 - Scatter Phase
Evaluates the physical reorganization of the array. The naive approach uses atomic shared cursors, causing intense cache line ping-ponging. The optimized strategy pre-calculates thread-local offsets to remove all memory synchronization.
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_scatter_Atomic -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_scatter -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_scatter_Atomic -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
```bash 
srun -w node07 --time=00:01:00 ./bin/hashjoin_par_partitioning_scatterBuffered -nr 20000000 -ns 20000000 -seed 42 -max-key 5000000 -p 256
``` 
Output to check: The _scatter_Atomic execution should be extremely slow due to thread stalling on memory synchronization. The optimized _scatter version should execute nearly 5x faster.

# EXPERIMENT 3: Scalability
Evaluates the overall application behavior as thread counts and workloads scale.
## HOW TO COMPILE
```bash 
make parallel_final
```
## EXPERIMENT 3.1 - Strong Scalability
Analyzes performance when the total problem size is kept constant while the number of threads increases.
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:02:00 ./strong_scalability.sh 
``` 
Output to check: log_detailed_strong_scalability.txt

## EXPERIMENT 3.2 - Weak Scalability
Evaluates the system by increasing the problem size proportionately with the number of threads. The objective is to maintain a strictly constant computational workload per thread across the algorithm's three primary kernels. Total records, total partitions, and the key range are all scaled up.
### HOW TO EXECUTE
```bash 
srun -w node07 --time=00:02:00 ./strong_scalability.sh 
``` 
Output to check: log_detailed_weak_scalability.txt




