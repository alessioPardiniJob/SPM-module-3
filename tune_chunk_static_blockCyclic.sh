#!/bin/bash

# ==============================================================================
# Parametri del Benchmark
# ==============================================================================
NR=20000000
NS=20000000
SEED=42
MAX_KEY=5000000
P=256
NODE="node07"   # Nodo di esecuzione
EXEC="./bin/hashjoin_par_localjoin_static_blockCyclic" # Il tuo eseguibile

# Valori di chunk size da testare (potenze di 2 fino a 64)
CHUNKS=(1 2 4 8 16 32 64)

# Colori per il terminale
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# File di output completo
LOG_FILE="log_chunk_tuning_static_blockcyclic.txt"
> $LOG_FILE # Pulisce il file se esiste già

echo -e "${BLUE}=== Avvio Tuning del Chunk Size (Block-Cyclic) ===${NC}"
echo -e "Parametri fissi: NR=$NR, NS=$NS, P=$P\n"

# ==============================================================================
# Esecuzione dei Test
# ==============================================================================

for C in "${CHUNKS[@]}"; do
    echo -e "${YELLOW}>> Eseguo Test con CHUNK = $C...${NC}"
    
    # Registra nel log quale test stiamo facendo
    echo "--- RUN WITH CHUNK = $C ---" >> $LOG_FILE
    
    # NOTA: Aggiungi "-policy bc" e "-t $THREADS" se il tuo eseguibile li richiede. 
    # Qui ho messo i parametri basandomi sul tuo input.
    srun -w $NODE --exclusive --time=00:01:00 $EXEC \
        -nr $NR -ns $NS -seed $SEED -max-key $MAX_KEY -p $P -chunk $C >> $LOG_FILE
    
    # Estrae a schermo i tempi della Local Join Phase dal log
    echo -e "${GREEN}Risultati (Chunk = $C):${NC}"
    grep -A 3 "JOIN PHASE" $LOG_FILE | tail -n 4
    echo "---------------------------------------------------"
done

echo -e "${BLUE}=== Tuning Completato! ===${NC}"
echo "Guarda la riga 'join_loop' qui sopra. Il valore di 'median' più basso indica il tuo Chunk Size ottimale."
echo "L'output completo di tutte le run è stato salvato in '$LOG_FILE'."