#!/bin/bash

# ==============================================================================
# Benchmark Parameters
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=10000000
P=256
NODE="node07"
EXEC="./bin/hashjoin_par_ompLoopLevel_AdaptiveMemoryTuning"

# ------------------------------------------------------------------------------
# Scheduling strategies to test
# static → chunk = p/num_threads (equal chunks, different from static, 1)
# dynamic → dynamic chunk, minimum 1
# driven → automatic initial chunk, minimum 1
# auto → let the runtime/compiler decide
# ------------------------------------------------------------------------------
SCHED_WITH_CHUNK=("static" "dynamic" "guided")
CHUNK_SIZES=(1 4 8 16 32 64 )

# ------------------------------------------------------------------------------
# Scheduling without chunk size (chunk=0 → OpenMP default)
# static → chunk = p/num_threads (equal chunks, different from static, 1)
# guided → automatic initial chunk, minimum 1
# auto → let the runtime/compiler decide
# ------------------------------------------------------------------------------
SCHED_NO_CHUNK=("static" "guided" "auto")

# Dataset configurations
declare -A DATASETS
DATASETS["uniform"]="-sigma 0   -subset-size 10 -crest-shape 2"
DATASETS["skewed"]="-sigma 0.95 -subset-size 10 -crest-shape 2"

# Terminal colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

LOG_FILE="log_scheduling_tuning.txt"
> "$LOG_FILE"

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}   Scheduling & Chunk Size Tuning — Uniform vs Skewed Dataset  ${NC}"
echo -e "${BLUE}================================================================${NC}"
echo -e "Fixed: NR=$NR, NS=$NS, P=$P, SEED=$SEED, MAX_KEY=$MAX_KEY\n"

# ==============================================================================
# Function: extract join_loop summary from log given a tag
# ==============================================================================
print_join_summary() {
    local tag="$1"
    awk -v t="$tag" '
        $0 ~ "=== RUN: "t" ===" { flag=1; next }
        flag && /^=== RUN:/ { flag=0 }
        flag && /join_loop/ { print "  join_loop |" $0 }
        flag && /build/     { print "  build     |" $0 }
        flag && /probe/     { print "  probe     |" $0 }
    ' "$LOG_FILE"
}

# ==============================================================================
# Function: execute a single run and log it
# Args: dataset_name  dataset_params  sched  chunk ("" = no chunk)
# ==============================================================================
run_test() {
    local dataset_name="$1"
    local dataset_params="$2"
    local sched="$3"
    local chunk="$4"

    local tag
    if [[ -n "$chunk" ]]; then
        tag="${dataset_name}__sched=${sched}__chunk=${chunk}"
    else
        tag="${dataset_name}__sched=${sched}__chunk=DEFAULT"
    fi

    local chunk_arg=""
    if [[ -n "$chunk" ]]; then
        chunk_arg="-chunk $chunk"
    fi

    echo -e "${YELLOW}>> [$dataset_name] sched=$sched chunk=${chunk:-DEFAULT}${NC}"
    echo "=== RUN: $tag ===" >> "$LOG_FILE"

    srun -w "$NODE" --exclusive --time=00:02:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P \
        $dataset_params \
        -sched "$sched" $chunk_arg >> "$LOG_FILE" 2>&1

    echo "" >> "$LOG_FILE"

    echo -e "${GREEN}  Risultati:${NC}"
    print_join_summary "$tag"
    echo "  ------------------------------------------"
}

# ==============================================================================
# Main loop
# ==============================================================================
for dataset_name in "uniform" "skewed"; do
    dataset_params="${DATASETS[$dataset_name]}"

    echo -e "\n${CYAN}================================================================${NC}"
    echo -e "${CYAN}  DATASET: ${dataset_name^^}${NC}"
    echo -e "${CYAN}================================================================${NC}"

    # --- Scheduling SENZA chunk (comportamento default OpenMP) ---
    echo -e "\n${CYAN}  -- Scheduling senza chunk esplicito (default OpenMP) --${NC}"
    for sched in "${SCHED_NO_CHUNK[@]}"; do
        run_test "$dataset_name" "$dataset_params" "$sched" ""
    done

    # --- Scheduling CON chunk esplicito ---
    echo -e "\n${CYAN}  -- Scheduling con chunk esplicito --${NC}"
    for sched in "${SCHED_WITH_CHUNK[@]}"; do
        for chunk in "${CHUNK_SIZES[@]}"; do
            run_test "$dataset_name" "$dataset_params" "$sched" "$chunk"
        done
    done

done

# ==============================================================================
# Final summary: extract join_loop median for all runs
# ==============================================================================
echo -e "\n${BLUE}================================================================${NC}"
echo -e "${BLUE}   RIEPILOGO FINALE — join_loop median (ms)${NC}"
echo -e "${BLUE}================================================================${NC}"

awk '
/^=== RUN:/ {
    tag = $0
    gsub(/^=== RUN: /, "", tag)
    gsub(/ ===/, "", tag)
}
/join_loop/ {
    # Estrae mean e std usando espressioni regolari dedicate
    m_match = match($0, /mean:[[:space:]]*([0-9]+\.[0-9]+)/, m_arr)
    s_match = match($0, /std:[[:space:]]*([0-9]+\.[0-9]+)/, s_arr)
    
    if (m_arr[1] != "" && s_arr[1] != "") {
        printf "%-62s | Mean: %8s ms | Std: %6s ms\n", tag, m_arr[1], s_arr[1]
    }
}
' "$LOG_FILE"

echo -e "\n${GREEN}=== Tuning completato! ===${NC}"
echo "Log completo salvato in '$LOG_FILE'."