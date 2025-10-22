#ifndef PE_H
#define PE_H

#include "cache.hpp"
#include "instr.h"
#include <vector>
#include <unordered_map>
#include <iostream>

// PROCESSING ELEMENT (PE)
class PE {
public:
    PE(int id, Cache* cache);
    
    // Gestion de programa
    void load_program(const std::vector<Instr>& prog,
                      const std::unordered_map<std::string,size_t>& labels);
    
    // Ejecucion
    void run();     // Ejecutar hasta HALT
    void step();    // Ejecutar una instruccion
    
    // Estado
    int get_pc() const { return pc; }
    bool is_halted() const { return halt_flag; }
    void dump_regs(std::ostream& os = std::cout) const;
    
    // Registros
    double get_reg_double(int r) const;
    void set_reg_double(int r, double v);
    int get_reg_int(int r) const;
    void set_reg_int(int r, int v);
    
    // Identificacion
    int pe_id() const { return id_; }
    
    // Control de ejecucion
    void set_pc(int new_pc) { pc = new_pc; halt_flag = false; }

    // Estadisticas simples del PE
    struct {
        uint64_t loads = 0;   // Conteo de instrucciones LOAD
        uint64_t stores = 0;  // Conteo de instrucciones STORE
    } stats;

private:
    // Ejecucion de instrucciones
    void exec_load(const Instr& I);
    void exec_store(const Instr& I);
    void exec_fmul(const Instr& I);
    void exec_fadd(const Instr& I);
    void exec_inc(const Instr& I);
    void exec_dec(const Instr& I);
    void exec_jnz(const Instr& I);

    // Datos miembros
    int id_;           // ID unico del PE
    Cache* cache_;     // Cache L1 privada
    int pc;            // Contador de programa
    bool halt_flag;    // Bandera de detencion
    double regs_raw[8]; // 8 registros de proposito general (como doubles)
    std::vector<Instr> program; // Programa cargado
    std::unordered_map<std::string,size_t> label_map; // Mapa de etiquetas
    
    static constexpr int DOUBLE_BYTES = 8; // Tamaño de double en bytes
};

#endif