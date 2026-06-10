# High-Performance Hash Join Optimization

## Introduction
This module evaluates the performance of a partitioned hash join implementation by comparing sequential execution with C++ threading and OpenMP-based parallel strategies (loop-level and task-level). The objective is to analyze scalability and robustness under uniform and skewed workloads


## Repository Structure
```text
.
├── Appendix.pdf
├── benchmarking_scheduling.sh
├── bin
│   ├── hashjoin_par_ompLoopLevel_AdaptiveMemory
│   └── hashjoin_par_ompTaskLevel_OvSubLocalJoin
├── Makefile
├── README.md
├── Skewed_data_generation.pdf
├── src
│   ├── hashjoin_par_cpp.cpp
│   ├── hashjoin_seq.cpp
│   ├── hashjoin_skewnessAnalysis.cpp
│   ├── omp_loop_level
│   │   ├── memoryManagment
│   │   │   ├── hashjoin_par_ompLoopLevel_1HM_x_partitions.cpp
│   │   │   ├── hashjoin_par_ompLoopLevel_AdaptiveMemory.cpp
│   │   │   └── hashjoin_par_ompLoopLevel.cpp
│   │   └── tuningScheduling
│   │       └── hashjoin_par_ompLoopLevel_AdaptiveMemoryTuning.cpp
│   └── omp_task_level
│       └── hashjoin_par_ompTaskLevel_OvSubLocalJoin.cpp
├── strong_scalabilityLoopLevel.sh
├── strong_scalabilityTaskLevel.sh
├── third_party
│   └── ska
│       ├── bytell_hash_map.hpp
│       ├── flat_hash_map.hpp
│       └── unordered_map.hpp
├── tune_bufferSize.sh
├── tuning_chunkSize.sh
├── utils
│   ├── affinity.hpp
│   ├── hpc_helpers.hpp
│   ├── taskFactory.hpp
│   ├── threadPool.hpp
│   └── utils.hpp
├── weak_scalabilityLoopLevel.sh
├── weak_scalabilityTaskLevel.sh  
└── bench_compare_all.sh
```
# EXPERIMENT 0 — Skewness Analysis
This initial experiment is intended to empirically verify that the data generation process is functioning correctly.
## HOW TO COMPILE
```bash 
make skewness_analysis
```
## HOW TO EXECUTE
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/skewness_analysis -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0 -subset-size 10 -crest-shape 2
```
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/skewness_analysis -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0.95 -subset-size 10 -crest-shape 2
```
Output to check: uniform_data.csv and skew_data.csv and terminal output 
# EXPERIMENT 1 — Loop Level Parallelism
This section contains all experiments related to loop-level parallelism.
## HOW TO COMPILE
```bash 
make parallel_omp_loop_level
make parallel_omp_loop_level_join_adaptive_memory
make parallel_omp_loop_level_join_1hm_x_partitions
make parallel_omp_loop_level_join_adaptive_memory_tuning_scheduling
```
## EXPERIMENT 1.1 - Memory Managment
This experiment evaluates different memory management strategies in the local join kernel under both uniform and skewed workload distributions. The goal is to assess how allocation policy and hash table reuse impact performance, particularly during the build and probe phases of the join operation.
### HOW TO EXECUTE
Hoisted (1 map/thread)
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0.95 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
1 map/partition
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel_1HM_x_partitions -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0.95 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel_1HM_x_partitions -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
Adaptive Memory
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel_AdaptiveMemory -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0.95 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
```bash 
srun -w node07 --exclusive --time=00:01:00 ./bin/hashjoin_par_ompLoopLevel_AdaptiveMemory -nr 20000000 -ns 20000000 -seed 42 -max-key 10000000 -p 256 -sigma 0 -subset-size 10 -crest-shape 2 -sched "dynamic" -chunk 8
```
Output to check: Look at the ========== BENCHMARK PROFILE ========== section for each run, in particular --- JOIN PHASE ---.
## EXPERIMENT 1.2 - Workload Scheduling Analysis
Compares different scheduling strategies and chunksize to handle workload balance.
### HOW TO EXECUTE
```bash 
./benchmarking_scheduling.sh
```
Output to check: Compare the final join_loop times.

# EXPERIMENT 2 — Task Level Parallelism
This section contains all experiments related to task-level parallelism.
## HOW TO COMPILE
```bash 
make parallel_omp_task_level_oversubscription_localjoin
```
## HOW TO EXECUTE
```bash 
./tuning_chunkSize.sh
```
Output to check: Look at the ">> [dataset_type] chunk-size=" section for each run or log_chunk_size_tuning.txt.

# EXPERIMENT 3: Scalability
Evaluates the overall application behavior as thread counts and workloads scale.
## HOW TO COMPILE
```bash 
make parallel_omp_task_level_oversubscription_localjoin
make parallel_omp_loop_level_join_adaptive_memory
```
## EXPERIMENT 3.1 - Strong Scalability
Analyzes performance when the total problem size is kept constant while the number of threads increases.
### HOW TO EXECUTE
```bash 
./strong_scalabilityLoopLevel.sh
./strong_scalabilityTaskLevel.sh 
```
Output to check: The results of the experiments are written to a detailed log file and two CSV files containing the measured performance under uniform and skewed workloads. For the task-level implementation, the output consists of log_detailed_strong_scalability_tasklevel.txt, strong_scalability_uniform_tasklevel.csv, and strong_scalability_skewed_tasklevel.csv. For the loop-level implementation, the corresponding outputs are log_detailed_strong_scalability_looplevel.txt, strong_scalability_uniform_looplevel.csv, and strong_scalability_skewed_looplevel.csv.

## EXPERIMENT 3.2 - Weak Scalability
Evaluates the system by increasing the problem size proportionately with the number of threads.
### HOW TO EXECUTE
```bash 
./weak_scalabilityLoopLevel.sh
./weak_scalabilityTaskLevel.sh 
```
Output to check: The results of the weak scalability experiments are stored in a detailed log file together with two CSV files reporting performance under uniform and skewed workloads. For the task-level implementation, the outputs are log_detailed_weak_scalability_tasklevel.txt, weak_scalability_uniform_tasklevel.csv, and weak_scalability_skewed_tasklevel.csv. For the loop-level implementation, the corresponding files are log_detailed_weak_scalability_looplevel.txt, weak_scalability_uniform_looplevel.csv, and weak_scalability_skewed_looplevel.csv.



# EXPERIMENT 4: Performance Comparison Across Sequential, C++ Threads, and OpenMP Implementations
Compare the execution times of the sequential, C++ Threads, OpenMP Loop-Level, and OpenMP Task-Level implementations on uniform and skewed datasets to assess scalability and robustness under workload imbalance.
## HOW TO COMPILE
```bash 
make seq
make parallel_cpp
make parallel_omp_task_level_oversubscription_localjoin
make parallel_omp_loop_level_join_adaptive_memory
```
### HOW TO EXECUTE
```bash 
./bench_compare_all.sh
```
Output to check: The benchmark generates two CSV files (comparison_uniform.csv and comparison_skewed.csv) containing the median execution times and standard deviations for each phase of the hash join algorithm. In particular, compare the Build, Probe, Join Loop, and Total execution times across the Sequential, C++ Threads, OpenMP Loop-Level, and OpenMP Task-Level implementations to evaluate scalability, load balancing, and robustness under both uniform and skewed data distributions.



