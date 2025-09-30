;Considering
;REG0: initial address of segment A for this PE
;REG1: initial address of segment B for this PE
;REG2: local accumulator address for this PE (partial_sums[ID])
;REG3: iteration counter (N/4)

LOAD REG 4, [REG2]		;Accumulates doubles (partial_sums[ID])
LOOP:
	LOAD REG5, [REG0]	;Load A[i] (double)
	LOAD REG6, [REG1]	;Load B[i] (double)
	FMUL REG7, REG5, REG6	;REG7 = A[i]*B[i] (double)
	FADD REG4, REG4, REG7	;REG4 += REG7 (double)	
	INC REG0		;Next element of A
	INC REG1		;Next element of B
	DEC REG3
	JNZ REG3, LOOP

STORE REG4, [REG2]		;Store the partial_sums (double)