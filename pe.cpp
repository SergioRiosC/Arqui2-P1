#include "pe.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <iomanip>

extern std::mutex io_mtx;

PE::PE(int id, Cache* cache) : id_(id), cache_(cache), pc(0), halt_flag(false) {
    for (int i = 0; i < 8; ++i) regs_raw[i] = 0.0;
}

void PE::load_program(const std::vector<Instr>& prog,
                      const std::unordered_map<std::string,size_t>& labels) {
    program = prog;
    label_map = labels;
    pc = 0;
    halt_flag = false;
}

void PE::run() {
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[PE" << id_ << "] run() START\n";
    }
    int steps = 0;
    while (!halt_flag && pc < (int)program.size()) {
        step();
        if (++steps % 100000 == 0) {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] still running, pc=" << pc << " steps=" << steps << "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[PE" << id_ << "] run() END pc=" << pc << " steps=" << steps << " halt=" << halt_flag << "\n";
    }
}

void PE::step() {
    Instr I = program[pc];
    switch (I.op) {
        case OpCode::LOAD:  exec_load(I); break;
        case OpCode::STORE: exec_store(I); break;
        case OpCode::FMUL:  exec_fmul(I); break;
        case OpCode::FADD:  exec_fadd(I); break;
        case OpCode::INC:   exec_inc(I); break;
        case OpCode::DEC:   exec_dec(I); break;
        case OpCode::JNZ:   exec_jnz(I); break;
        case OpCode::HALT:  halt_flag = true; break;
        default: break;
    }
    pc++;
}

void PE::exec_load(const Instr& I) {
    uint64_t addr = I.addr_is_reg ? static_cast<uint64_t>(get_reg_int(I.ra)) : static_cast<uint64_t>(I.address);
    if (addr % DOUBLE_BYTES != 0) {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cerr << "[WARN][PE" << id_ << "] access not 8B-aligned addr=" << addr 
                << " (instr pc=" << pc << " rd=R" << I.rd << ")\n";
    }
    double v = cache_->read_double(addr);
    set_reg_double(I.rd, v);
    stats.loads++;
}

void PE::exec_store(const Instr& I) {
    uint64_t addr = I.addr_is_reg ? static_cast<uint64_t>(get_reg_int(I.ra)) : static_cast<uint64_t>(I.address);
    double val = get_reg_double(I.rd);
    if (addr % DOUBLE_BYTES != 0) {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cerr << "[WARN][PE" << id_ << "] access not 8B-aligned addr=" << addr 
                << " (instr pc=" << pc << " rd=R" << I.rd << ")\n";
    }

    cache_->write_double(addr, val);
    stats.stores++;
}

void PE::exec_fmul(const Instr& I) {
    set_reg_double(I.rd, get_reg_double(I.ra) * get_reg_double(I.rb));
}

void PE::exec_fadd(const Instr& I) {
    set_reg_double(I.rd, get_reg_double(I.ra) + get_reg_double(I.rb));
}

void PE::exec_inc(const Instr& I) {
    set_reg_int(I.rd, get_reg_int(I.rd) + DOUBLE_BYTES);
}

void PE::exec_dec(const Instr& I) { 
    set_reg_int(I.rd, get_reg_int(I.rd) - 1); 
}

void PE::exec_jnz(const Instr& I) {
    if (get_reg_int(I.rd) != 0) {
        auto it = label_map.find(I.label);
        if (it != label_map.end()) pc = int(it->second) - 1;
    }
}

void PE::dump_regs(std::ostream& os) const {
    std::lock_guard<std::mutex> lk(io_mtx);
    os << "[PE" << id_ << "] PC=" << pc << " HALT=" << halt_flag << "\n";
    for (int i = 0; i < 8; ++i) {
        os << "  R" << i << " = " << get_reg_double(i) << "\n";
    }
}

double PE::get_reg_double(int r) const { 
    return regs_raw[r]; 
}

void PE::set_reg_double(int r, double v) { 
    regs_raw[r] = v; 
}

int PE::get_reg_int(int r) const { 
    return static_cast<int>(regs_raw[r]); 
}

void PE::set_reg_int(int r, int v) { 
    regs_raw[r] = static_cast<double>(v); 
}