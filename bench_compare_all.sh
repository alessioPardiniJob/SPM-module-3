#!/bin/bash

# ==============================================================================
# Algorithm Comparison Parameters
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=10000000 
P=256
NODE="node07"
THREADS=32

# Executables
EXEC_SEQ="./bin/hashjoin_seq"
EXEC_CXX="./bin/hashjoin_par_cpp"
EXEC_LOOP="./bin/hashjoin_par_ompLoopLevel_AdaptiveMemory"
EXEC_TASK="./bin/hashjoin_par_ompTaskLevel_OvSubLocalJoin"

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
LOG_FILE="log_detailed_comparison.txt"
CSV_UNIFORM="comparison_uniform.csv"
CSV_SKEWED="comparison_skewed.csv"

# Construct the CSV header (The first column is now the Implementation)
HEADER="Implementation,"
HEADER+="HistR,HistR_std,ScatR,ScatR_std,"
HEADER+="HistS,HistS_std,ScatS,ScatS_std,"
HEADER+="Build,Build_std,Probe,Probe_std,"
HEADER+="JoinLoop,JoinLoop_std,Total,Total_std"

# Initialize both CSV files with the header
echo "$HEADER" > "$CSV_UNIFORM"
echo "$HEADER" > "$CSV_SKEWED"
> "$LOG_FILE" # Clear the log file at startup

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}   Algorithm Comparison — Uniform vs Skewed (Median + Std)      ${NC}"
echo -e "${BLUE}================================================================${NC}"
echo -e "Fixed: NR=$NR, NS=$NS, P=$P, SEED=$SEED, MAX_KEY=$MAX_KEY, THREADS=$THREADS\n"

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

    # Array to iterate over the implementations
    IMPLEMENTATIONS=("Sequential" "C++_Threads" "OMP_Loop" "OMP_Task")

    for impl in "${IMPLEMENTATIONS[@]}"; do
        echo -ne "${YELLOW}>> [$dataset_name] Running $impl... ${NC}"
        
        # Build the specific command based on the implementation
        case $impl in
            "Sequential")
                CMD="srun -w $NODE --exclusive $EXEC_SEQ -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P $dataset_params"
                ;;
            "C++_Threads")
                CMD="srun -w $NODE --exclusive $EXEC_CXX -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk 16 -t $THREADS $dataset_params"
                ;;
            "OMP_Loop")
                CMD="srun -w $NODE --exclusive $EXEC_LOOP -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -sched dynamic -chunk 1 -t $THREADS $dataset_params"
                ;;
            "OMP_Task")
                CMD="srun -w $NODE --exclusive $EXEC_TASK -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk-size 1 -t $THREADS $dataset_params"
                ;;
        esac

        # Execute the binary and capture the output
        RUN_OUTPUT=$(eval $CMD)

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

        # Fallback handling in case the sequential version lacks certain metric formats
        if [ -z "$RES_TOTAL" ]; then
            RES_TOTAL="N/A,N/A"
        fi

        # Append to the specific CSV file
        echo "$impl,$RES_HIST_R,$RES_SCAT_R,$RES_HIST_S,$RES_SCAT_S,$RES_BUILD,$RES_PROBE,$RES_LOOP,$RES_TOTAL" >> "$CURRENT_CSV"
        
        # Isolate the total median time for console feedback
        TOTAL_VAL=$(echo "$RES_TOTAL" | cut -d',' -f1)
        echo -e "${GREEN}OK! (Total: ${TOTAL_VAL}ms)${NC}"
        
        # Append detailed execution logs
        echo "=== RUN: ${dataset_name^^} | IMPL: $impl ===" >> "$LOG_FILE"
        echo "$RUN_OUTPUT" >> "$LOG_FILE"
        echo -e "\n" >> "$LOG_FILE"
    done
    echo "" # Space between datasets
done

echo -e "${BLUE}=== Test Completed! ===${NC}"
echo "Full log saved in        : $LOG_FILE"
echo "Uniform CSV saved in     : $CSV_UNIFORM"
echo "Skewed CSV saved in      : $CSV_SKEWED"
echo "-------------------------------------------------------"
echo -e "${CYAN}--- UNIFORM Preview ---${NC}"
column -t -s, "$CSV_UNIFORM"
echo ""
echo -e "${CYAN}--- SKEWED Preview ---${NC}"
column -t -s, "$CSV_SKEWED"