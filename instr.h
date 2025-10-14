#ifndef INSTR_H
#define INSTR_H

#include <string>

enum class OpCode { LOAD, STORE, FMUL, FADD, INC, DEC, JNZ, NOP, HALT };

struct Instr {
    OpCode op;
    int rd, ra, rb;
    bool addr_is_reg;
    size_t address;
    std::string label;

    Instr(OpCode o=OpCode::NOP)
        : op(o), rd(0), ra(-1), rb(-1), addr_is_reg(false), address(0), label("") {}
};

#endif
