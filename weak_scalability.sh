#!/bin/bash

# ==============================================================================
# Parametri BASE Weak Scalability (Valori per T=1)
# ==============================================================================
BASE_NR=2000000
BASE_NS=2000000
SEED=42
BASE_MAX_KEY=500000
BASE_P=64

NODE="node07"
EXEC="./bin/hashjoin_par_final"
BEST_CHUNK=16 

# Lista di thread da testare. 
# NOTA: Usiamo solo potenze di 2 per garantire che CURRENT_P sia sempre potenza di 2!
THREADS_LIST=(1 2 4 8 16 32 64 128)

# Colori
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

LOG_FILE="log_detailed_weak_scalability.txt"
CSV_FILE="weak_scalability_results.csv"

# Intestazione CSV
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
    # Calcolo dei parametri scalati per il Weak Scaling (Parametro_Base * T)
    # --------------------------------------------------------------------------
    CURRENT_NR=$((BASE_NR * T))
    CURRENT_NS=$((BASE_NS * T))
    CURRENT_MAX_KEY=$((BASE_MAX_KEY * T))
    CURRENT_P=$((BASE_P * T))

    echo -ne "${YELLOW}>> Esecuzione con $T Thread (N=$CURRENT_NR, K=$CURRENT_MAX_KEY, P=$CURRENT_P)... ${NC}"
    
    # Esecuzione con i parametri dinamicamente calcolati
    RUN_OUTPUT=$(srun -w $NODE --exclusive $EXEC \
        -nr $CURRENT_NR -ns $CURRENT_NS -seed $SEED -max-key $CURRENT_MAX_KEY -p $CURRENT_P -chunk $BEST_CHUNK -t $T)

    # --- Funzione di estrazione ---
    # $4 è la mediana, $12 è lo std
    extract_pair() {
        local section_content="$1"
        local label="$2"
        echo "$section_content" | grep "$label" | awk '{print $4 "," $12}'
    }

    # Sezionamento dell'output per evitare collisioni di nomi
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
    
    # Log dettagliato
    echo "--- THREADS: $T | N: $CURRENT_NR | P: $CURRENT_P | K: $CURRENT_MAX_KEY ---" >> $LOG_FILE
    echo "$RUN_OUTPUT" >> $LOG_FILE
    echo "" >> $LOG_FILE
done

echo -e "\n${BLUE}=== Test Completato! ===${NC}"
echo "Dati salvati in: $CSV_FILE"
echo "Log output in: $LOG_FILE"
echo "-------------------------------------------------------"
column -t -s, $CSV_FILE