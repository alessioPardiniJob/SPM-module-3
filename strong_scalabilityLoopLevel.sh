#!/bin/bash

# ==============================================================================
# Strong Scalability Parameters
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=10000000
P=256
NODE="node07"
EXEC="./bin/hashjoin_par_ompLoopLevel_AdaptiveMemory"
SCHED="dynamic"
BEST_CHUNK=1 

# Array containing the sequence of thread counts to be evaluated
THREADS_LIST=(1 4 8 16 32 64 128 256)

# ==============================================================================
# Dataset Configurations
# ==============================================================================
declare -A DATASETS
DATASETS["uniform"]="-sigma 0 -subset-size 10 -crest-shape 2"
DATASETS["skewed"]="-sigma 0.95 -subset-size 10 -crest-shape 2"

# ANSI escape codes for terminal color formatting
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# Output file definitions for logs and results
LOG_FILE="log_detailed_strong_scalability_looplevel.txt"
CSV_UNIFORM="strong_scalability_uniform_looplevel.csv"
CSV_SKEWED="strong_scalability_skewed_looplevel.csv"

# Construct the CSV header
HEADER="Threads,"
HEADER+="HistR,HistR_std,ScatR,ScatR_std,"
HEADER+="HistS,HistS_std,ScatS,ScatS_std,"
HEADER+="Build,Build_std,Probe,Probe_std,"
HEADER+="JoinLoop,JoinLoop_std,Total,Total_std"

# Initialize both CSV files with the header
echo "$HEADER" > "$CSV_UNIFORM"
echo "$HEADER" > "$CSV_SKEWED"
> "$LOG_FILE" # Azzera il file di log all'avvio

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}   Strong Scalability Test — Uniform vs Skewed (Median + Std)   ${NC}"
echo -e "${BLUE}================================================================${NC}"
echo -e "Fixed: NR=$NR, NS=$NS, P=$P, SEED=$SEED, MAX_KEY=$MAX_KEY, SCHED=$SCHED, CHUNK=$BEST_CHUNK\n"

# --- Metric Extraction Function ---
# Extracts the 4th column (median) and the 12th column (standard deviation)
extract_pair() {
    local section_content="$1"
    local label="$2"
    echo "$section_content" | grep "$label" | awk '{print $4 "," $12}'
}

# ==============================================================================
# Main Execution Loop
# ==============================================================================
for dataset_name in "uniform" "skewed"; do
    dataset_params="${DATASETS[$dataset_name]}"
    
    # Assign the correct CSV file for the current dataset
    if [ "$dataset_name" == "uniform" ]; then
        CURRENT_CSV="$CSV_UNIFORM"
    else
        CURRENT_CSV="$CSV_SKEWED"
    fi

    echo -e "${CYAN}================================================================${NC}"
    echo -e "${CYAN}  DATASET: ${dataset_name^^}${NC}"
    echo -e "${CYAN}================================================================${NC}"

    # Iterate through the specified thread counts
    for T in "${THREADS_LIST[@]}"; do
        echo -ne "${YELLOW}>> [$dataset_name] Testing with $T Thread(s)... ${NC}"
        
        # Execute the binary with dataset parameters
        RUN_OUTPUT=$(srun -w "$NODE" --exclusive $EXEC \
            -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -sched $SCHED -chunk $BEST_CHUNK -t $T \
            $dataset_params)

        # Segment the raw output to prevent label collisions
        PART_R=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION R/,/PARTITION S/p')
        PART_S=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION S/,/JOIN PHASE/p')
        JOIN_P=$(echo "$RUN_OUTPUT" | sed -n '/JOIN PHASE/,/GLOBAL/p')
        GLOBAL_P=$(echo "$RUN_OUTPUT" | sed -n '/GLOBAL/,$p')

        # Extract comma-separated pairs of (median, std)
        RES_HIST_R=$(extract_pair "$PART_R" "histogram")
        RES_SCAT_R=$(extract_pair "$PART_R" "scatter")
        
        RES_HIST_S=$(extract_pair "$PART_S" "histogram")
        RES_SCAT_S=$(extract_pair "$PART_S" "scatter")
        
        RES_BUILD=$(extract_pair "$JOIN_P" "build")
        RES_PROBE=$(extract_pair "$JOIN_P" "probe")
        RES_LOOP=$(extract_pair "$JOIN_P" "join_loop")
        
        RES_TOTAL=$(extract_pair "$GLOBAL_P" "TOTAL")

        # Append to the specific CSV file
        echo "$T,$RES_HIST_R,$RES_SCAT_R,$RES_HIST_S,$RES_SCAT_S,$RES_BUILD,$RES_PROBE,$RES_LOOP,$RES_TOTAL" >> "$CURRENT_CSV"
        
        # Isolate the total median time for console feedback
        TOTAL_VAL=$(echo "$RES_TOTAL" | cut -d',' -f1)
        echo -e "${GREEN}OK! (Total: ${TOTAL_VAL}ms)${NC}"
        
        # Append detailed execution logs
        echo "=== RUN: ${dataset_name^^} | THREADS: $T ===" >> "$LOG_FILE"
        echo "$RUN_OUTPUT" >> "$LOG_FILE"
        echo -e "\n" >> "$LOG_FILE"
    done
    echo "" # Spazio tra un dataset e l'altro
done

echo -e "${BLUE}=== Test Completed! ===${NC}"
echo "Log completo salvato in     : $LOG_FILE"
echo "Dati CSV Uniform salvati in : $CSV_UNIFORM"
echo "Dati CSV Skewed salvati in  : $CSV_SKEWED"
echo "-------------------------------------------------------"
echo -e "${CYAN}--- Anteprima UNIFORM ---${NC}"
column -t -s, "$CSV_UNIFORM"
echo ""
echo -e "${CYAN}--- Anteprima SKEWED ---${NC}"
column -t -s, "$CSV_SKEWED"