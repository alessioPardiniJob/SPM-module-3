#!/bin/bash

# ==============================================================================
# Weak Scalability Parameters (Based on 1-thread baseline)
# ==============================================================================
BASE_NR=2000000
BASE_NS=2000000
BASE_MAX_KEY=500000
BASE_P=64 
SEED=42
NODE="node07"
EXEC="./bin/hashjoin_par_ompTaskLevel_OvSubLocalJoin"
BEST_CHUNK=1 

# Lista dei thread (Scaling factor)
THREADS_LIST=(1 4 8 16 32 64)

# ==============================================================================
# Dataset Configurations (Morphology fixed, don't scale these!)
# ==============================================================================
declare -A DATASETS
DATASETS["uniform"]="-sigma 0 -subset-size 10 -crest-shape 2"
DATASETS["skewed"]="-sigma 0.95 -subset-size 10 -crest-shape 2"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

LOG_FILE="log_weak_scalability_TaskLevel.txt"
CSV_UNIFORM="weak_scalability_uniform_tasklevel.csv"
CSV_SKEWED="weak_scalability_skewed_tasklevel.csv"

# Header
HEADER="Threads,N,P,MaxKey,HistR,HistR_std,ScatR,ScatR_std,HistS,HistS_std,ScatS,ScatS_std,Build,Build_std,Probe,Probe_std,JoinLoop,JoinLoop_std,Total,Total_std"

echo "$HEADER" > "$CSV_UNIFORM"
echo "$HEADER" > "$CSV_SKEWED"
> "$LOG_FILE"

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}        Weak Scalability Test (Volume scales with T)           ${NC}"
echo -e "${BLUE}================================================================${NC}"

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
    CURRENT_CSV=$( [ "$dataset_name" == "uniform" ] && echo "$CSV_UNIFORM" || echo "$CSV_SKEWED" )

    echo -e "${CYAN}--- DATASET: ${dataset_name^^} ---${NC}"

    for T in "${THREADS_LIST[@]}"; do
        # Calcolo dei parametri scalati
        CURR_NR=$((BASE_NR * T))
        CURR_NS=$((BASE_NS * T))
        CURR_MAX_KEY=$((BASE_MAX_KEY * T))
        CURR_P=$((BASE_P * T))

        echo -ne "${YELLOW}>> T=$T (N=$CURR_NR, P=$CURR_P, K=$CURR_MAX_KEY)... ${NC}"
        
        # Esecuzione
        RUN_OUTPUT=$(srun -w "$NODE" --exclusive $EXEC \
            -nr $CURR_NR -ns $CURR_NS -seed $SEED -max-key $CURR_MAX_KEY -p $CURR_P -chunk-size $BEST_CHUNK -t $T \
            $dataset_params)

        # Estrazione dati
        PART_R=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION R/,/PARTITION S/p')
        PART_S=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION S/,/JOIN PHASE/p')
        JOIN_P=$(echo "$RUN_OUTPUT" | sed -n '/JOIN PHASE/,/GLOBAL/p')
        GLOBAL_P=$(echo "$RUN_OUTPUT" | sed -n '/GLOBAL/,$p')

        RES_HIST_R=$(extract_pair "$PART_R" "histogram")
        RES_SCAT_R=$(extract_pair "$PART_R" "scatter")
        RES_HIST_S=$(extract_pair "$PART_S" "histogram")
        RES_SCAT_S=$(extract_pair "$PART_S" "scatter")
        RES_BUILD=$(extract_pair "$JOIN_P" "build")
        RES_PROBE=$(extract_pair "$JOIN_P" "probe")
        RES_LOOP=$(extract_pair "$JOIN_P" "join_loop")
        RES_TOTAL=$(extract_pair "$GLOBAL_P" "TOTAL")

        # Scrittura CSV (includiamo N, P e K per tracciabilità nel CSV)
        echo "$T,$CURR_NR,$CURR_P,$CURR_MAX_KEY,$RES_HIST_R,$RES_SCAT_R,$RES_HIST_S,$RES_SCAT_S,$RES_BUILD,$RES_PROBE,$RES_LOOP,$RES_TOTAL" >> "$CURRENT_CSV"
        
        echo -e "${GREEN}OK!${NC}"
        echo "=== RUN: ${dataset_name^^} | T: $T | N: $CURR_NR ===" >> "$LOG_FILE"
        echo "$RUN_OUTPUT" >> "$LOG_FILE"
    done
done