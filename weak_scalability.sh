#!/bin/bash

# ==============================================================================
# Base Parameters for Weak Scalability (Values corresponding to T=1)
# ==============================================================================
BASE_NR=2000000
BASE_NS=2000000
SEED=42
BASE_MAX_KEY=500000
BASE_P=64

NODE="node07"
EXEC="./bin/hashjoin_par_final"
BEST_CHUNK=16 

# List of thread counts to be evaluated. 
# NOTE: Only powers of 2 are utilized to guarantee that CURRENT_P consistently remains a power of 2!
THREADS_LIST=(1 2 4 8 16 32 64 128)

# Terminal color configurations for output formatting
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

LOG_FILE="log_detailed_weak_scalability.txt"
CSV_FILE="weak_scalability_results.csv"

# CSV File Header Configuration
HEADER="Threads,"
HEADER+="HistR,HistR_std,ScatR,ScatR_std,"
HEADER+="HistS,HistS_std,ScatS,ScatS_std,"
HEADER+="Build,Build_std,Probe,Probe_std,"
HEADER+="JoinLoop,JoinLoop_std,Total,Total_std"

echo "$HEADER" > $CSV_FILE

echo -e "${BLUE}=== Weak Scalability Test (Median + StdDev) ===${NC}"
echo -e "Scaling: N, MAX_KEY e P crescono linearmente con i Thread.\n"

for T in "${THREADS_LIST[@]}"; do

    # --------------------------------------------------------------------------
    # Computation of scaled parameters for Weak Scaling (Base_Parameter * T)
    # --------------------------------------------------------------------------
    CURRENT_NR=$((BASE_NR * T))
    CURRENT_NS=$((BASE_NS * T))
    CURRENT_MAX_KEY=$((BASE_MAX_KEY * T))
    CURRENT_P=$((BASE_P * T))

    echo -ne "${YELLOW}>> Execution with $T Thread (N=$CURRENT_NR, K=$CURRENT_MAX_KEY, P=$CURRENT_P)... ${NC}"
    
    # Job execution via SLURM using the dynamically computed parameters
    RUN_OUTPUT=$(srun -w $NODE --exclusive $EXEC \
        -nr $CURRENT_NR -ns $CURRENT_NS -seed $SEED -max-key $CURRENT_MAX_KEY -p $CURRENT_P -chunk $BEST_CHUNK -t $T)

    # --- Data Extraction Function ---
    # Extracts the median (field $4) and standard deviation (field $12) from the output
    extract_pair() {
        local section_content="$1"
        local label="$2"
        echo "$section_content" | grep "$label" | awk '{print $4 "," $12}'
    }

    # Segmenting the execution output to prevent metric name collisions across different execution phases
    PART_R=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION R/,/PARTITION S/p')
    PART_S=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION S/,/JOIN PHASE/p')
    JOIN_P=$(echo "$RUN_OUTPUT" | sed -n '/JOIN PHASE/,/GLOBAL/p')
    GLOBAL_P=$(echo "$RUN_OUTPUT" | sed -n '/GLOBAL/,$p')

    # Extracting the (median, standard deviation) data pairs for each metric
    RES_HIST_R=$(extract_pair "$PART_R" "histogram")
    RES_SCAT_R=$(extract_pair "$PART_R" "scatter")
    
    RES_HIST_S=$(extract_pair "$PART_S" "histogram")
    RES_SCAT_S=$(extract_pair "$PART_S" "scatter")
    
    RES_BUILD=$(extract_pair "$JOIN_P" "build")
    RES_PROBE=$(extract_pair "$JOIN_P" "probe")
    RES_LOOP=$(extract_pair "$JOIN_P" "join_loop")
    
    RES_TOTAL=$(extract_pair "$GLOBAL_P" "TOTAL")

    # Appending the extracted metrics as a new row in the CSV file
    echo "$T,$RES_HIST_R,$RES_SCAT_R,$RES_HIST_S,$RES_SCAT_S,$RES_BUILD,$RES_PROBE,$RES_LOOP,$RES_TOTAL" >> $CSV_FILE
    
    # Extracting solely the total median value to provide real-time console feedback
    TOTAL_VAL=$(echo $RES_TOTAL | cut -d',' -f1)
    echo -e "${GREEN}OK! (Total: ${TOTAL_VAL}ms)${NC}"
    
    # Appending detailed execution parameters and raw output to the log file
    echo "--- THREADS: $T | N: $CURRENT_NR | P: $CURRENT_P | K: $CURRENT_MAX_KEY ---" >> $LOG_FILE
    echo "$RUN_OUTPUT" >> $LOG_FILE
    echo "" >> $LOG_FILE
done

echo -e "\n${BLUE}=== Test Completed! ===${NC}"
echo "Data saved in: $CSV_FILE"
echo "Log output in: $LOG_FILE"
echo "-------------------------------------------------------"
column -t -s, $CSV_FILE