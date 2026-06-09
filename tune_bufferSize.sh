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
EXEC="./bin/hashjoin_par_cpp" # Target executable

# Array of chunk sizes to evaluate (powers of 2 up to 64)
BUFFER_SIZES=(8 16 32 64 128 256)

# Terminal color definitions for standard output formatting
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No color/reset

# Comprehensive output log file definition
LOG_FILE="log_bufferSize_tuning.txt"
> $LOG_FILE # Initializes the log file by clearing previous contents if it exists

echo -e "${BLUE}=== Starting Buffer Size Tuning ===${NC}"
echo -e "Fixed parameters: NR=$NR, NS=$NS, P=$P\n"

# ==============================================================================
# Test Execution Phase
# ==============================================================================

for C in "${BUFFER_SIZES[@]}"; do
    echo -e "${YELLOW}>> Starting Test with BUFFER SIZE = $C...${NC}"
    
    # Records the current buffer size configuration to the log file
    echo "--- RUN WITH BUFFER SIZE = $C ---" >> $LOG_FILE
    
    
    srun -w $NODE --exclusive --time=00:01:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -buffer $C >> $LOG_FILE
    
   echo -e "${GREEN}Results (Buffer Size = $C):${NC}"

    awk -v bs="$C" '
    $0 ~ "--- RUN WITH BUFFER SIZE = "bs" ---" {flag=1; next}
    $0 ~ "--- RUN WITH BUFFER SIZE =" && flag {flag=0}

    flag && /^--- PARTITION R ---/ {mode="R"; print "\nPARTITION R"}
    flag && /^--- PARTITION S ---/ {mode="S"; print "\nPARTITION S"}

    flag && mode=="R" && NF {print}
    flag && mode=="S" && NF {print}
    ' $LOG_FILE

    echo "---------------------------------------------------"
done

echo -e "${BLUE}=== Tuning Completed! ===${NC}"
echo "Results saved in '$LOG_FILE'."