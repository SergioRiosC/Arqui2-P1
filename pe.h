#ifndef PE_H
#define PE_H

#include "cache.hpp"
#include "instr.h"
#include <vector>
#include <unordered_map>
#include <iostream>

class PE {
public:
    PE(int id, Cache* cache);
    void load_program(const std::vector<Instr>& prog,
                      const std::unordered_map<std::string,size_t>& labels);
    void run();
    void step();
    int get_pc() const { return pc; }
    bool is_halted() const { return halt_flag; }
    void dump_regs(std::ostream& os = std::cout) const;
    double get_reg_double(int r) const;
    void set_reg_double(int r, double v);
    int get_reg_int(int r) const;
    void set_reg_int(int r, int v);
    int pe_id() const { return id_; }

    struct {
        uint64_t loads = 0;
        uint64_t stores = 0;
    } stats;

private:
    void exec_load(const Instr& I);
    void exec_store(const Instr& I);
    void exec_fmul(const Instr& I);
    void exec_fadd(const Instr& I);
    void exec_inc(const Instr& I);
    void exec_dec(const Instr& I);
    void exec_jnz(const Instr& I);

    int id_;
    Cache* cache_;
    int pc;
    bool halt_flag;
    double regs_raw[8];
    std::vector<Instr> program;
    std::unordered_map<std::string,size_t> label_map;
    
    static constexpr int DOUBLE_BYTES = 8;
};

#endif