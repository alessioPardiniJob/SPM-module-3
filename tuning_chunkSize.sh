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
EXEC="./bin/hashjoin_par_ompTaskLevel_OvSubLocalJoin"

# ------------------------------------------------------------------------------
# Chunk Sizes to test (in terms of number of partitions per task)
# ------------------------------------------------------------------------------
CHUNK_SIZES=(1 4 8 16 32 64 128 256)

# Dataset configurations
# Uniform: sigma = 0 (subset-size e crest-shape non influenzano la distribuzione se sigma=0)
# Skewed:  sigma = 0.95, subset-size = 10, crest-shape = 2 (da esempio srun)
declare -A DATASETS
DATASETS["uniform"]="-sigma 0 -subset-size 10 -crest-shape 2"
DATASETS["skewed"]="-sigma 0.95 -subset-size 10 -crest-shape 2"

# Terminal colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

LOG_FILE="log_chunk_size_tuning.txt"
> "$LOG_FILE"

echo -e "${BLUE}================================================================${NC}"
echo -e "${BLUE}   Chunk Size Tuning — Uniform vs Skewed Dataset         ${NC}"
echo -e "${BLUE}================================================================${NC}"
echo -e "Fixed: NR=$NR, NS=$NS, P=$P, SEED=$SEED, MAX_KEY=$MAX_KEY\n"

# ==============================================================================
# Funzione: estrae i dati del log dato un tag (focalizzato sulla join)
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
# Funzione: esegue un singolo run e lo logga
# Args: dataset_name  dataset_params  chunk_size
# ==============================================================================
run_test() {
    local dataset_name="$1"
    local dataset_params="$2"
    local chunk_size="$3"

    local tag="${dataset_name}__chunk-size=${chunk_size}"

    echo -e "${YELLOW}>> [$dataset_name] chunk-size=$chunk_size${NC}"
    echo "=== RUN: $tag ===" >> "$LOG_FILE"

    # Esecuzione srun con i parametri corretti per il task-level
    srun -w "$NODE" --exclusive --time=00:02:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P \
        $dataset_params \
        -chunk-size $chunk_size >> "$LOG_FILE" 2>&1

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

    # Iterazione sui vari chunk sizes
    for chunk_size in "${CHUNK_SIZES[@]}"; do
        run_test "$dataset_name" "$dataset_params" "$chunk_size"
    done
done

# ==============================================================================
# Riepilogo finale — join_loop median per ogni combinazione
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
    # Estrae mean e std usando le espressioni regolari del log originale
    m_match = match($0, /mean:[[:space:]]*([0-9]+\.[0-9]+)/, m_arr)
    s_match = match($0, /std:[[:space:]]*([0-9]+\.[0-9]+)/, s_arr)
    
    if (m_arr[1] != "" && s_arr[1] != "") {
        printf "%-50s | Mean: %8s ms | Std: %6s ms\n", tag, m_arr[1], s_arr[1]
    }
}
' "$LOG_FILE"

echo -e "\n${GREEN}=== Tuning completato! ===${NC}"
echo "Log completo salvato in '$LOG_FILE'."