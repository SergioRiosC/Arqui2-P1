# Producto punto parcial
LOAD R4, [R2]        # acumulador inicial
LOOP:
    LOAD R5, [R0]    # A[i]
    LOAD R6, [R1]    # B[i]
    FMUL R7, R5, R6
    FADD R4, R4, R7
    INC R0
    INC R1
    DEC R3
    JNZ R3, LOOP
STORE R4, [R2]
HALT
