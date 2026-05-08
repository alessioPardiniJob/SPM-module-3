#!/bin/bash

# ==============================================================================
# Benchmark Parameters
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=5000000
P=256
NODE="node07"   # Target execution node
EXEC="./bin/hashjoin_par_localjoin_dynamic_threadPool" # Target executable

# Array of chunk sizes to evaluate (powers of 2 up to 64)
CHUNKS=(1 2 4 8 16 32 64)

# Terminal color definitions for standard output formatting
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No color/reset

# Comprehensive output log file definition
LOG_FILE="log_chunk_tuning_dynamic_threadPool.txt"
> $LOG_FILE # Initializes the log file by clearing previous contents if it exists

echo -e "${BLUE}=== Starting Chunk Size Tuning (Dynamic Thread Pool) ===${NC}"
echo -e "Fixed parameters: NR=$NR, NS=$NS, P=$P\n"

# ==============================================================================
# Test Execution Phase
# ==============================================================================

for C in "${CHUNKS[@]}"; do
    echo -e "${YELLOW}>> Starting Test with CHUNK = $C...${NC}"
    
    # Records the current chunk size configuration to the log file
    echo "--- RUN WITH CHUNK = $C ---" >> $LOG_FILE
    
    
    srun -w $NODE --exclusive --time=00:01:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk $C >> $LOG_FILE
    
    # Parses and outputs the execution metrics for the Local Join Phase from the log
    echo -e "${GREEN}Results (Chunk = $C):${NC}"
    grep -A 3 "JOIN PHASE" $LOG_FILE | tail -n 4
    echo "---------------------------------------------------"
done

echo -e "${BLUE}=== Tuning Completed! ===${NC}"
echo "Results saved in '$LOG_FILE'."