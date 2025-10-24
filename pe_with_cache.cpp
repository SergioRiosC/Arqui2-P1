// pe_with_cache.cpp
#include <iostream>
#include <vector>
#include <unordered_map>
#include <thread>
#include <fstream>
#include <sstream>
#include <mutex>
#include <memory>
#include <iomanip>

#include "pe.h"
#include "cache.hpp"
#include "parser.h"   
#include "instr.h"    
#include "shared_memory.h"
#include "shared_memory_adapter.h"


#include <atomic>
#include <chrono>



//static std::mutex io_mtx;
static std::atomic<int> pe_started{0};
static std::atomic<int> pe_finished{0};
// ---------------- PE ----------------
/*
class PE {
public:
    // ahora el cache es el tipo Cache definido en cache.hpp
    PE(int id, Cache* cache): id_(id), cache_(cache), pc(0), halt_flag(false) {
        for (int i = 0; i < 8; ++i) regs_raw[i] = 0.0;
    }

    void load_program(const std::vector<Instr>& prog,
                      const std::unordered_map<std::string,size_t>& labels) {
        program = prog;
        label_map = labels;
        pc = 0;
        halt_flag = false;
    }

    void run() {
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[PE" << id_ << "] run() START\n";
    }
    pe_started.fetch_add(1);
    int steps = 0;
    while (!halt_flag && pc < (int)program.size()) {
        step();
        if (++steps % 100000 == 0) { // cada 100k instrucciones imprime algo
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] still running, pc=" << pc << " steps=" << steps << "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[PE" << id_ << "] run() END pc=" << pc << " steps=" << steps << " halt=" << halt_flag << "\n";
    }
    pe_finished.fetch_add(1);
}
    // --- PE getters para stepping ---
    int get_pc() const { return pc; }
    bool is_halted() const { return halt_flag; }

    // Imprime registros
    void dump_regs(std::ostream& os = std::cout) const {
        std::lock_guard<std::mutex> lk(io_mtx);
        os << "[PE" << id_ << "] PC=" << pc << " HALT=" << halt_flag << "\n";
        for (int i = 0; i < 8; ++i) {
            os << "  R" << i << " = " << get_reg_double(i) << "\n";
        }
    }


    double get_reg_double(int r) const { return regs_raw[r]; }
    void set_reg_double(int r, double v) { regs_raw[r] = v; }
    int get_reg_int(int r) const { return static_cast<int>(regs_raw[r]); }
    void set_reg_int(int r, int v) { regs_raw[r] = static_cast<double>(v); }

    // Exponer stats locales (ligeros, pero la cache tiene sus propios stats)
    struct {
        uint64_t loads = 0;
        uint64_t stores = 0;
    } stats;

private:
    void step() {
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

    void exec_load(const Instr& I) {
        uint64_t addr = I.addr_is_reg ? static_cast<uint64_t>(get_reg_int(I.ra)) : static_cast<uint64_t>(I.address);
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] LOAD before addr=" << addr << " rd=R" << I.rd << "\n";
        }
        if (addr % DOUBLE_BYTES != 0) {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cerr << "[WARN][PE" << id_ << "] access not 8B-aligned addr=" << addr 
                    << " (instr pc=" << pc << " rd=R" << I.rd << ")\n";
        }

        double v = cache_->read_double(addr);
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] LOAD after addr=" << addr << " got=" << v << " -> R" << I.rd << "\n";
        }
        set_reg_double(I.rd, v);
        stats.loads++;
    }

    void exec_store(const Instr& I) {
        uint64_t addr = I.addr_is_reg ? static_cast<uint64_t>(get_reg_int(I.ra)) : static_cast<uint64_t>(I.address);
        double val = get_reg_double(I.rd);
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] STORE before addr=" << addr << " val=" << val << " from R" << I.rd << "\n";
        }
        if (addr % DOUBLE_BYTES != 0) {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cerr << "[WARN][PE" << id_ << "] access not 8B-aligned addr=" << addr 
                    << " (instr pc=" << pc << " rd=R" << I.rd << ")\n";
        }

        cache_->write_double(addr, val);
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[PE" << id_ << "] STORE after addr=" << addr << "\n";
        }
        stats.stores++;
    }

    void exec_fmul(const Instr& I) {
        set_reg_double(I.rd, get_reg_double(I.ra) * get_reg_double(I.rb));
    }
    void exec_fadd(const Instr& I) {
        set_reg_double(I.rd, get_reg_double(I.ra) + get_reg_double(I.rb));
    }
    // Tamano en bytes de un double en este sistema
    static constexpr int DOUBLE_BYTES = 8;

    // INC: en este ISA se usa para avanzar el puntero al siguiente elemento (double).
    void exec_inc(const Instr& I){
        // asumimos que los registros que se incrementan con INC son punteros en bytes
        set_reg_int(I.rd, get_reg_int(I.rd) + DOUBLE_BYTES);
    }

    void exec_dec(const Instr& I) { set_reg_int(I.rd, get_reg_int(I.rd) - 1); }
    void exec_jnz(const Instr& I) {
        if (get_reg_int(I.rd) != 0) {
            auto it = label_map.find(I.label);
            if (it != label_map.end()) pc = int(it->second) - 1;
        }
    }

    int id_;
    Cache* cache_;                    // apuntador a la cache L1 de este PE
    int pc;
    bool halt_flag;
    double regs_raw[8];
    std::vector<Instr> program;
    std::unordered_map<std::string,size_t> label_map;
};*/

// ---------------- Main ----------------
#ifndef STEPPER_APP
int main(int argc, char** argv) {
    using namespace hw;

    // -------- parametros --------
    int N = 8;                  // por defecto
    constexpr int P = 4;        // SIEMPRE 4 PEs
    if (argc >= 2) N = std::max(1, std::atoi(argv[1]));

    // Layout: A[0..N-1], B[0..N-1], S[0..P-1]
    const size_t baseA_words = 0;
    const size_t baseB_words = baseA_words + static_cast<size_t>(N);
    const size_t baseS_words = baseB_words + static_cast<size_t>(N);
    const size_t needed_words = baseS_words + P;

    // -------- memoria compartida + adaptador --------
    SharedMemory shm(static_cast<uint32_t>(std::max<size_t>(needed_words, hw::kMemDoubles)));
    // Opcional: segmentar 4 regiones (no obligatorio para que funcione)
    shm.add_segment(0, 0,                 static_cast<uint32_t>(needed_words/4 + 1));
    shm.add_segment(1, static_cast<uint32_t>(needed_words/4 + 1), static_cast<uint32_t>(needed_words/4 + 1));
    shm.add_segment(2, static_cast<uint32_t>(2*(needed_words/4 + 1)), static_cast<uint32_t>(needed_words/4 + 1));
    shm.add_segment(3, static_cast<uint32_t>(3*(needed_words/4 + 1)), static_cast<uint32_t>(needed_words/4 + 1));
    shm.start();

    SharedMemoryAdapter mem(&shm);   // <- este es el "Memory" real para la cache
    Interconnect bus;

    // Inicializa A y B via adaptador (byte addresses)
    for (int i = 0; i < N; ++i) {
        mem.store64((baseA_words + i) * 8ull, double(i + 1));        // A[i]
        mem.store64((baseB_words + i) * 8ull, double((i + 1) * 2));  // B[i]
    }
    // Inicializa S
    for (int p = 0; p < P; ++p) mem.store64((baseS_words + p) * 8ull, 0.0);

    // -------- caches y PEs --------
    std::vector<std::unique_ptr<Cache>> caches;
    std::vector<std::unique_ptr<PE>> pes;
    caches.reserve(P); pes.reserve(P);
    for (int i = 0; i < P; ++i) {
        caches.emplace_back(std::make_unique<Cache>(i, &mem, &bus));
        pes.emplace_back(std::make_unique<PE>(i, caches.back().get()));
    }

    // -------- programa --------
    std::ifstream fin("dotprod.asm");
    if (!fin) { std::cerr << "Error: no se pudo abrir dotprod.asm\n"; return 1; }
    std::stringstream buffer; buffer << fin.rdbuf();
    std::vector<Instr> prog; std::unordered_map<std::string,size_t> labels;
    parse_asm(buffer.str(), prog, labels);

    // -------- reparto balanceado con resto --------
    const int base_len = N / P;
    const int rest = N % P;
    auto start_index_of = [&](int pe) { return pe * base_len + std::min(pe, rest); };
    auto len_of         = [&](int pe) { return base_len + (pe < rest ? 1 : 0); };

    for (int p = 0; p < P; ++p) {
        const int start = start_index_of(p);
        const int len   = len_of(p);
        pes[p]->load_program(prog, labels);
        pes[p]->set_reg_int(0, int((baseA_words + start) * 8)); // &A[start] bytes
        pes[p]->set_reg_int(1, int((baseB_words + start) * 8)); // &B[start] bytes
        pes[p]->set_reg_int(2, int((baseS_words + p) * 8));     // &S[p] bytes
        pes[p]->set_reg_int(3, len);                            // longitud tramo
        pes[p]->set_reg_double(4, 0.0);                         // acumulador
    }

    // -------- ejecutar --------
    std::vector<std::thread> threads;
    for (int p = 0; p < P; ++p) threads.emplace_back([&pes, p](){ pes[p]->run(); });
    for (auto &t : threads) t.join();

    bus.flush_all();   // <- garantiza que DRAM tiene los ultimos valores

    // Flush para asegurar write-back antes de leer S
    for (auto &c : caches) c->flush_all();

    // -------- resultados --------
    for (int p = 0; p < P; ++p) {
        uint64_t addrS = (baseS_words + p) * 8ull;
        std::cout << "PE" << p << " sum stored at M[" << (baseS_words + p)
                  << "] = " << mem.load64(addrS) << "\n";
    }

    double total = 0.0;
    for (int p = 0; p < P; ++p) total += mem.load64((baseS_words + p) * 8ull);

    double expected = 0.0;
    for (int i = 0; i < N; ++i)
        expected += mem.load64((baseA_words + i) * 8ull) * mem.load64((baseB_words + i) * 8ull);

    std::cout << "\nProducto punto (reduccion final) = " << total << "\n";
    std::cout << "Producto punto (esperado secuencial) = " << expected << "\n\n";

    std::cout << "Estadisticas por Cache (por PE):\n";
    for (int p = 0; p < P; ++p) {
        const auto &s = caches[p]->stats();
        std::cout << "PE" << p << ": reads=" << s.read_ops
                  << " writes=" << s.write_ops
                  << " misses=" << s.misses
                  << " invalidations=" << s.invalidations
                  << " bus_msgs=" << s.bus_msgs << "\n";
    }

    shm.stop(); // detener el hilo de la memoria compartida
    return 0;
}
#endif
