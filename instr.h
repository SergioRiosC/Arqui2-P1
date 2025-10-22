#ifndef INSTR_H
#define INSTR_H

#include <string>
#include <cstdint>

// CODIGOS DE OPERACIÓN - Conjunto de instrucciones soportado
enum class OpCode {
    NOP,    // No operation
    LOAD,   // Carga desde memoria a registro
    STORE,  // Almacena desde registro a memoria  
    FMUL,   // Multiplicación de punto flotante
    FADD,   // Suma de punto flotante
    INC,    // Incrementar registro (para punteros)
    DEC,    // Decrementar registro (para contadores)
    JNZ,    // Salto condicional si no es cero
    HALT    // Terminar ejecución
};

// ESTRUCTURA DE INSTRUCCIÓN - Representa una instrucción decodificada
struct Instr {
    OpCode op = OpCode::NOP; // Código de operación
    
    // Campos de registro (dependen de la instrucción)
    int rd = 0;  // Registro destino
    int ra = 0;  // Registro operando A  
    int rb = 0;  // Registro operando B
    
    // Campos de dirección/memoria
    bool addr_is_reg = false; // True si la dirección viene de registro
    size_t address = 0;       // Dirección inmediata (si addr_is_reg es false)
    
    // Campo para saltos
    std::string label;        // Etiqueta destino para saltos
};

#endif