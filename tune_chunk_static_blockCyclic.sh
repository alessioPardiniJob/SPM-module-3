#!/bin/bash

# ==============================================================================
# Benchmark Parameters
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=5000000
P=256
NODE="node07"   # Execution node
EXEC="./bin/hashjoin_par_localjoin_static_blockCyclic" # Target executable

# Chunk sizes to be evaluated (powers of 2 up to 64)
CHUNKS=(1 2 4 8 16 32 64)

# Terminal color codes
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Comprehensive output log file
LOG_FILE="log_chunk_tuning_static_blockcyclic.txt"
> $LOG_FILE # Clears the log file if it already exists

echo -e "${BLUE}=== Starting Chunk Size Tuning (Block-Cyclic) ===${NC}"
echo -e "Fixed parameters: NR=$NR, NS=$NS, P=$P\n"

# ==============================================================================
# Test Execution
# ==============================================================================

for C in "${CHUNKS[@]}"; do
    echo -e "${YELLOW}>> Starting Test with CHUNK = $C...${NC}"
    
    # Logs the current test configuration
    echo "--- RUN WITH CHUNK = $C ---" >> $LOG_FILE
    
    # NOTE: Append "-policy bc" and "-t $THREADS" if required by the target executable. 
    # The current parameters are based on the provided input.
    srun -w $NODE --exclusive --time=00:01:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk $C >> $LOG_FILE
    
    # Extracts and displays the Local Join Phase execution times from the log file
    echo -e "${GREEN}Results (Chunk = $C):${NC}"
    grep -A 3 "JOIN PHASE" $LOG_FILE | tail -n 4
    echo "---------------------------------------------------"
done

echo -e "${BLUE}=== Tuning Completed! ===${NC}"
echo "Results saved in '$LOG_FILE'."