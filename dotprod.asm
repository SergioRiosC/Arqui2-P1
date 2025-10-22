# Codigo para calculo de producto punto
# Registros: R0-R7

# --- Configuracion de registros por PE ---
# R0: direccion inicial segmento A
# R1: direccion inicial segmento B  
# R2: direccion suma parcial S[ID]
# R3: contador de iteraciones
# R4: acumulador
# R5-R7: temporales

# --- Codigo para calculo parcial ---
MAIN:
    LOAD R4, [R2]        # Carga acumulador inicial (S[ID])
    
LOOP:
    LOAD R5, [R0]        # Carga A[i]
    LOAD R6, [R1]        # Carga B[i]
    FMUL R7, R5, R6      # R7 = A[i] * B[i]
    FADD R4, R4, R7      # Acumula en R4
    INC R0               # Siguiente elemento A (+8 bytes)
    INC R1               # Siguiente elemento B (+8 bytes)
    DEC R3               # Decrementa contador
    JNZ R3, LOOP         # Salta si no es cero
    
    STORE R4, [R2]       # Guarda suma parcial
    HALT

# --- Codigo para suma final (ejecutado por PE0) ---
FINAL_SUM:
    # R0: direccion base sumas parciales
    # R1: numero de PEs
    # R2: direccion resultado final
    # R3-R7: temporales
    
    LOAD R4, [R0]        # Carga S[0]
    LOAD R3, 1           # Inicia contador PE en 1
    
SUM_LOOP:
    INC R0               # Siguiente suma parcial
    LOAD R5, [R0]        # Carga S[i]
    FADD R4, R4, R5      # Suma al acumulador
    INC R3               # Incrementa contador PE
    DEC R1               # Decrementa contador total
    JNZ R1, SUM_LOOP     # Continua si quedan PEs
    
    STORE R4, [R2]       # Guarda resultado final
    HALT
