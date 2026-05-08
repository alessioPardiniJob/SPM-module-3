CXX        := g++
CXXFLAGS   := -std=c++20 -Wall
OPTFLAGS   := -O3 -fno-tree-vectorize
INCLUDES := -I. -Ithird_party -Iutils

SRC_DIR    := src
BIN_DIR    := bin

TARGET_UNORDERED 									:= $(BIN_DIR)/hashjoin_seq_unordered_map
TARGET_FLAT      									:= $(BIN_DIR)/hashjoin_seq_flat_map
TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKBASED      	:= $(BIN_DIR)/hashjoin_par_localjoin_static_blockBased
TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC		:= $(BIN_DIR)/hashjoin_par_localjoin_static_blockCyclic
TARGET_PARALLEL_LOCALJOIN_DYNAMIC_THREADPOOL		:= $(BIN_DIR)/hashjoin_par_localjoin_dynamic_threadPool


TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC_OPTIMIZED := $(BIN_DIR)/hashjoin_par_localjoin_static_blockCyclic_Optimized



TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_GLOBALATOMIC					:= $(BIN_DIR)/hashjoin_par_partitioning_compute_histogram_globalAtomic
TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL							:= $(BIN_DIR)/hashjoin_par_partitioning_compute_histogram_local
TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_FLATARRAY					:= $(BIN_DIR)/hashjoin_par_partitioning_compute_histogram_local_flatArrayPadding
TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_THREADAFFINITY			:= $(BIN_DIR)/hashjoin_par_partitioning_compute_histogram_local_threadAffinity

TARGET_PARALLEL_PARTITIONING_SCATTER_GLOBALATOMIC					:= $(BIN_DIR)/hashjoin_par_partitioning_scatter_Atomic
TARGET_PARALLEL_PARTITIONING_SCATTER								:= $(BIN_DIR)/hashjoin_par_partitioning_scatter
TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED						:= $(BIN_DIR)/hashjoin_par_partitioning_scatterBuffered
TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED_FIRSTTOUCH			:= $(BIN_DIR)/hashjoin_par_partitioning_scatterBufferedFirstTouch

TARGET_PARALLEL_FINAL := $(BIN_DIR)/hashjoin_par_final


COMMON_FLAGS := $(CXXFLAGS) $(OPTFLAGS) $(INCLUDES)

.PHONY: all clean directories unordered flat parallel_localjoin_static_blockbased	parallel_localjoin_static_blockcyclic parallel_localjoin_dynamic_threadpool parallel_localjoin_static_blockcyclic_optimized parallel_partitioning_compute_histogram_globalatomic parallel_partitioning_compute_histogram_local parallel_partitioning_compute_histogram_local_flatarray parallel_partitioning_compute_histogram_local_threadaffinity parallel_partitioning_scatter_globalatomic parallel_partitioning_scatter parallel_partitioning_scatter_buffered parallel_partitioning_scatter_buffered_firsttouch parallel_final

# Default: build everything
all: directories $(TARGET_UNORDERED) $(TARGET_FLAT) $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKBASED) $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC) $(TARGET_PARALLEL_LOCALJOIN_DYNAMIC_THREADPOOL) $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC_OPTIMIZED) $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_GLOBALATOMIC) $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL) $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_FLATARRAY) $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_THREADAFFINITY) $(TARGET_PARALLEL_PARTITIONING_SCATTER_GLOBALATOMIC) $(TARGET_PARALLEL_PARTITIONING_SCATTER) $(TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED) $(TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED_FIRSTTOUCH) $(TARGET_PARALLEL_FINAL)

unordered: directories $(TARGET_UNORDERED)
flat: directories $(TARGET_FLAT)
parallel_localjoin_static_blockbased: directories $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKBASED)
parallel_localjoin_static_blockcyclic: directories $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC)
parallel_localjoin_dynamic_threadpool: directories $(TARGET_PARALLEL_LOCALJOIN_DYNAMIC_THREADPOOL)
parallel_localjoin_static_blockcyclic_optimized: directories $(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC_OPTIMIZED)

parallel_partitioning_compute_histogram_globalatomic: directories $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_GLOBALATOMIC)
parallel_partitioning_compute_histogram_local: directories $(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL)

parallel_partitioning_scatter_globalatomic: directories $(TARGET_PARALLEL_PARTITIONING_SCATTER_GLOBALATOMIC)
parallel_partitioning_scatter: directories $(TARGET_PARALLEL_PARTITIONING_SCATTER)
parallel_partitioning_scatter_buffered: directories $(TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED)


parallel_final : directories $(TARGET_PARALLEL_FINAL)
# -------- Directories --------
directories:
	mkdir -p $(BIN_DIR)

# -------- Targets --------

$(TARGET_UNORDERED): $(SRC_DIR)/hashjoin_seq_unordered_map.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_FLAT): $(SRC_DIR)/hashjoin_seq_flat_map.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKBASED): $(SRC_DIR)/Parallelization/localJoin/hashjoin_par_localjoin_static_blockBased.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC): $(SRC_DIR)/Parallelization/localJoin/hashjoin_par_localjoin_static_blockCyclic.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_LOCALJOIN_DYNAMIC_THREADPOOL): $(SRC_DIR)/Parallelization/localJoin/hashjoin_par_localjoin_dynamic_threadPool.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_LOCALJOIN_STATIC_BLOCKCYCLIC_OPTIMIZED): $(SRC_DIR)/Parallelization/localJoin/hashjoin_par_localjoin_static_blockCyclic_Optimized.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_GLOBALATOMIC): $(SRC_DIR)/Parallelization/partitioning/compute_histogram/hashjoin_par_partitioning_compute_histogram_globalAtomic.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL): $(SRC_DIR)/Parallelization/partitioning/compute_histogram/hashjoin_par_partitioning_compute_histogram_local.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_FLATARRAY): $(SRC_DIR)/Parallelization/partitioning/compute_histogram/hashjoin_par_partitioning_compute_histogram_local_flatArrayPadding.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

$(TARGET_PARALLEL_PARTITIONING_COMPUTEHISTOGRAM_LOCAL_THREADAFFINITY): $(SRC_DIR)/Parallelization/partitioning/compute_histogram/hashjoin_par_partitioning_compute_histogram_local_threadAffinity.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<


$(TARGET_PARALLEL_PARTITIONING_SCATTER_GLOBALATOMIC): $(SRC_DIR)/Parallelization/partitioning/scatter/hashjoin_par_partitioning_scatter_Atomic.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<


$(TARGET_PARALLEL_PARTITIONING_SCATTER): $(SRC_DIR)/Parallelization/partitioning/scatter/hashjoin_par_partitioning_scatter.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<
$(TARGET_PARALLEL_PARTITIONING_SCATTER_BUFFERED): $(SRC_DIR)/Parallelization/partitioning/scatter/hashjoin_par_partitioning_scatterBuffered.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<



$(TARGET_PARALLEL_FINAL): $(SRC_DIR)/hashjoin_par_final.cpp
	$(CXX) $(COMMON_FLAGS) -o $@ $<

# Clean
clean:
	rm -rf $(BIN_DIR)