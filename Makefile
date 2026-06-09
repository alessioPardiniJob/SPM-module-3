CXX        := g++
CXXFLAGS   := -std=c++20 -Wall
OPTFLAGS   := -O3 -fno-tree-vectorize
INCLUDES := -I. -Ithird_party -Iutils
OMPFLAGS   := -fopenmp

SRC_DIR    := src
BIN_DIR    := bin

TARGET_SEQ      										:= $(BIN_DIR)/hashjoin_seq
TARGET_PARALLEL_CPP 									:= $(BIN_DIR)/hashjoin_par_cpp
TARGET_SKEWNESS_ANALYSIS 								:= $(BIN_DIR)/skewness_analysis
TARGET_PARALLEL_OMP_LOOP_LEVEL 							:= $(BIN_DIR)/hashjoin_par_ompLoopLevel
TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY 	:= $(BIN_DIR)/hashjoin_par_ompLoopLevel_AdaptiveMemory
TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_1HM_X_PARTITIONS 	:= $(BIN_DIR)/hashjoin_par_ompLoopLevel_1HM_x_partitions
TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY_TUNING_SCHEDULING := $(BIN_DIR)/hashjoin_par_ompLoopLevel_AdaptiveMemoryTuning
TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_PARTITIONING		:= $(BIN_DIR)/hashjoin_par_ompTaskLevel_OvSubPartitioningPhase
TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_LOCALJOIN		:= $(BIN_DIR)/hashjoin_par_ompTaskLevel_OvSubLocalJoin


COMMON_FLAGS := $(CXXFLAGS) $(OPTFLAGS) $(INCLUDES)

.PHONY: all clean directories seq parallel_cpp parallel_omp_loop_level parallel_omp_loop_level_join_adaptive_memory parallel_omp_loop_level_join_1hm_x_partitions parallel_omp_task_level_oversubscription_partitioning skewness_analysis parallel_omp_task_level_oversubscription_localjoin parallel_omp_loop_level_join_adaptive_memory_tuning_scheduling

# Default: build everything
all: directories $(TARGET_SEQ) $(TARGET_PARALLEL_CPP) $(TARGET_PARALLEL_OMP_LOOP_LEVEL) $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY) $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_1HM_X_PARTITIONS) $(TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_PARTITIONING) $(TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_LOCALJOIN) $(TARGET_SKEWNESS_ANALYSIS) $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY_TUNING_SCHEDULING)

seq : directories $(TARGET_SEQ)

skewness_analysis : directories $(TARGET_SKEWNESS_ANALYSIS)

parallel_cpp : directories $(TARGET_PARALLEL_CPP)

parallel_omp_loop_level : directories $(TARGET_PARALLEL_OMP_LOOP_LEVEL)

parallel_omp_loop_level_join_adaptive_memory : directories $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY)

parallel_omp_loop_level_join_1hm_x_partitions : directories $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_1HM_X_PARTITIONS)

parallel_omp_loop_level_join_adaptive_memory_tuning_scheduling : directories $(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY_TUNING_SCHEDULING)

parallel_omp_task_level_oversubscription_localjoin : directories $(TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_LOCALJOIN)

# -------- Directories --------
directories:
	mkdir -p $(BIN_DIR)

# -------- Targets --------

$(TARGET_SEQ): $(SRC_DIR)/hashjoin_seq.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_SKEWNESS_ANALYSIS): $(SRC_DIR)/hashjoin_skewnessAnalysis.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<	

$(TARGET_PARALLEL_CPP): $(SRC_DIR)/hashjoin_par_cpp.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_OMP_LOOP_LEVEL): $(SRC_DIR)/omp_loop_level/memoryManagment/hashjoin_par_ompLoopLevel.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<

$(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY): $(SRC_DIR)/omp_loop_level/memoryManagment/hashjoin_par_ompLoopLevel_AdaptiveMemory.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<


$(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_1HM_X_PARTITIONS): $(SRC_DIR)/omp_loop_level/memoryManagment/hashjoin_par_ompLoopLevel_1HM_x_partitions.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<

$(TARGET_PARALLEL_OMP_LOOP_LEVEL_JOIN_ADAPTIVE_MEMORY_TUNING_SCHEDULING): $(SRC_DIR)/omp_loop_level/tuningScheduling/hashjoin_par_ompLoopLevel_AdaptiveMemoryTuning.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<


$(TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_PARTITIONING): $(SRC_DIR)/omp_task_level/hashjoin_par_ompTaskLevel_OvSubPartitioningPhase.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<

$(TARGET_PARALLEL_OMP_TASK_LEVEL_OVERSUBSCRIPTION_LOCALJOIN): $(SRC_DIR)/omp_task_level/hashjoin_par_ompTaskLevel_OvSubLocalJoin.cpp
	$(CXX) $(COMMON_FLAGS) $(OMPFLAGS) -o $@ $<

# Clean
clean:
	rm -rf $(BIN_DIR)