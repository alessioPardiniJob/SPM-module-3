#!/bin/bash

# ==============================================================================
# Parametri Strong Scalability
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=5000000
P=256
NODE="node07"
EXEC="./bin/hashjoin_par_final"
BEST_CHUNK=16 

# Lista di thread da testare
THREADS_LIST=(1 2 3 4 6 8 10 12 16 20 24 32 40 48 56 64)
# Colori
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

LOG_FILE="log_detailed_scalability.txt"
CSV_FILE="strong_scalability_results.csv"

# Intestazione CSV: ogni metrica ha ora la sua colonna _std
HEADER="Threads,"
HEADER+="HistR,HistR_std,ScatR,ScatR_std,"
HEADER+="HistS,HistS_std,ScatS,ScatS_std,"
HEADER+="Build,Build_std,Probe,Probe_std,"
HEADER+="JoinLoop,JoinLoop_std,Total,Total_std"

echo "$HEADER" > $CSV_FILE

echo -e "${BLUE}=== Strong Scalability Test (Median + StdDev) ===${NC}"

for T in "${THREADS_LIST[@]}"; do
    echo -ne "${YELLOW}>> Esecuzione con $T Thread... ${NC}"
    
    RUN_OUTPUT=$(srun -w $NODE --exclusive $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk $BEST_CHUNK -t $T)

    # --- Funzione di estrazione ---
    # $4 è la mediana, $12 è lo std (basato sul tuo output)
    extract_pair() {
        local section_content="$1"
        local label="$2"
        echo "$section_content" | grep "$label" | awk '{print $4 "," $12}'
    }

    # Sezionamento dell'output per evitare collisioni di nomi tra R e S
    PART_R=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION R/,/PARTITION S/p')
    PART_S=$(echo "$RUN_OUTPUT" | sed -n '/PARTITION S/,/JOIN PHASE/p')
    JOIN_P=$(echo "$RUN_OUTPUT" | sed -n '/JOIN PHASE/,/GLOBAL/p')
    GLOBAL_P=$(echo "$RUN_OUTPUT" | sed -n '/GLOBAL/,$p')

    # Estrazione coppie (valore,std)
    RES_HIST_R=$(extract_pair "$PART_R" "histogram")
    RES_SCAT_R=$(extract_pair "$PART_R" "scatter")
    
    RES_HIST_S=$(extract_pair "$PART_S" "histogram")
    RES_SCAT_S=$(extract_pair "$PART_S" "scatter")
    
    RES_BUILD=$(extract_pair "$JOIN_P" "build")
    RES_PROBE=$(extract_pair "$JOIN_P" "probe")
    RES_LOOP=$(extract_pair "$JOIN_P" "join_loop")
    
    RES_TOTAL=$(extract_pair "$GLOBAL_P" "TOTAL")

    # Scrittura riga CSV
    echo "$T,$RES_HIST_R,$RES_SCAT_R,$RES_HIST_S,$RES_SCAT_S,$RES_BUILD,$RES_PROBE,$RES_LOOP,$RES_TOTAL" >> $CSV_FILE
    
    # Estraiamo solo il valore della mediana totale per il feedback a schermo
    TOTAL_VAL=$(echo $RES_TOTAL | cut -d',' -f1)
    echo -e "${GREEN}OK! (Total: ${TOTAL_VAL}ms)${NC}"
    
    echo "--- THREADS: $T ---" >> $LOG_FILE
    echo "$RUN_OUTPUT" >> $LOG_FILE
done

echo -e "\n${BLUE}=== Test Completato! ===${NC}"
echo "Dati salvati in: $CSV_FILE"
echo "-------------------------------------------------------"
column -t -s, $CSV_FILE