#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <optional>
#include <unordered_set>
#include <cctype>
#include <memory>
#include <fstream>
#include <sstream>

#include "cache.hpp"
#include "shared_memory_adapter.h"
#include "shared_memory.h"
#include "parser.h"
#include "instr.h"
#include "pe.h"

// ---------- Utilidad pequena de parsing ----------
static inline std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream is(s);
    std::vector<std::string> v;
    std::string tok;
    while (is >> tok) v.push_back(tok);
    return v;
}

static inline bool to_uint64(const std::string& s, uint64_t& out) {
    try {
        size_t p=0;
        int base = (s.rfind("0x",0)==0 || s.rfind("0X",0)==0) ? 16 : 10;
        out = std::stoull(s, &p, base);
        return p == s.size();
    } catch (...) { return false; }
}

static inline bool to_int(const std::string& s, int& out) {
    try { size_t p=0; out = std::stoi(s, &p, 10); return p==s.size(); }
    catch(...) { return false; }
}

// ---------- Construccion del "sistema" ----------
struct System {
    std::shared_ptr<SharedMemory> shm;
    std::unique_ptr<SharedMemoryAdapter> mem;
    Interconnect bus;
    std::vector<std::unique_ptr<Cache>> l1;
    std::vector<std::unique_ptr<PE>> pes;
    
    // Constructor que inicializa todo correctamente
    System(unsigned num_pes, int N = 8) {  // Agregar N como parametro
        // Crear memoria compartida
        shm = std::make_shared<SharedMemory>(512);
        shm->start();
        
        // Crear adaptador de memoria
        mem = std::make_unique<SharedMemoryAdapter>(shm.get());
        
        // Crear caches
        l1.reserve(num_pes);
        for (unsigned i = 0; i < num_pes; ++i) {
            l1.emplace_back(std::make_unique<Cache>(int(i), mem.get(), &bus));
        }
        
        // Crear PEs
        pes.reserve(num_pes);
        for (unsigned i = 0; i < num_pes; ++i) {
            pes.emplace_back(std::make_unique<PE>(int(i), l1[i].get()));
        }
        
        // Inicializar memoria y cargar programa
        initialize_memory(N);
        load_program_to_all_pes(N);
    }
    
    void initialize_memory(int N) {
        // Layout: A[0..N-1], B[0..N-1], S[0..P-1]
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        // Inicializar vectores A y B
        for (int i = 0; i < N; ++i) {
            mem->store64((baseA_words + i) * 8, double(i + 1));           // A[i] = i+1
            mem->store64((baseB_words + i) * 8, double((i + 1) * 2));     // B[i] = (i+1)*2
        }
        
        // Inicializar sumas parciales
        for (unsigned p = 0; p < pes.size(); ++p) {
            mem->store64((baseS_words + p) * 8, 0.0);
        }
    }
    
    void load_program_to_all_pes(int N) {
        // Cargar programa desde archivo
        std::ifstream fin("dotprod.asm");
        if (!fin) {
            std::cerr << "Error: no se pudo abrir dotprod.asm\n";
            return;
        }
        
        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::vector<Instr> prog;
        std::unordered_map<std::string,size_t> labels;
        parse_asm(buffer.str(), prog, labels);
        
        // Layout de memoria
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        const unsigned num_pes = pes.size();
        
        // Reparto balanceado con resto
        const int base_len = N / num_pes;
        const int rest = N % num_pes;
        auto start_index_of = [&](int pe) { return pe * base_len + std::min(pe, rest); };
        auto len_of         = [&](int pe) { return base_len + (pe < rest ? 1 : 0); };
        
        // Configurar cada PE
        for (unsigned p = 0; p < num_pes; ++p) {
            const int start = start_index_of(p);
            const int len   = len_of(p);
            
            pes[p]->load_program(prog, labels);
            pes[p]->set_reg_int(0, int((baseA_words + start) * 8));  // &A[start] bytes
            pes[p]->set_reg_int(1, int((baseB_words + start) * 8));  // &B[start] bytes
            pes[p]->set_reg_int(2, int((baseS_words + p) * 8));      // &S[p] bytes
            pes[p]->set_reg_int(3, len);                             // longitud tramo
            pes[p]->set_reg_double(4, 0.0);                          // acumulador
        }
    }
    
    ~System() {
        if (shm) {
            shm->stop();
        }
    }
};

// ---------- REPL ----------
struct Breakpoint {
    int pe_id;
    int pc;
    bool operator==(const Breakpoint& o) const { return pe_id==o.pe_id && pc==o.pc; }
};

struct BkHash { 
    size_t operator()(const Breakpoint& b) const { 
        return (size_t(b.pe_id)<<20) ^ size_t(b.pc); 
    } 
};

static void print_help() {
    std::cout <<
R"(Comandos:
  help                       - ayuda
  step [N]                   - avanza N instrucciones globales (RR) (default 1)
  stepi <pe> [N]             - avanza N instrucciones solo en PE <pe> (default 1)
  cont                       - ejecuta hasta que todos halteen o haya breakpoint
  regs [pe]                  - muestra registros (todos si omites pe)
  pc [pe]                    - muestra PC(s)
  mem <addr> [count]         - lee memoria como dobles desde <addr> (hex o dec). count por defecto 8
  cache [pe]                 - dump del estado de cache de <pe>
  stats                      - estadisticas de todas las caches
  break <pe> <pc>            - pone breakpoint en PC de ese PE
  breaks                     - lista breakpoints
  clear <pe> <pc>            - quita un breakpoint
  quit                       - salir
)" << std::endl;
}

static bool any_running(const std::vector<std::unique_ptr<PE>>& pes) {
    for (auto& p : pes) if (!p->is_halted()) return true;
    return false;
}

static bool hit_breakpoint(const std::vector<std::unique_ptr<PE>>& pes,
                           const std::unordered_set<Breakpoint,BkHash>& bks) {
    for (auto& p : pes) {
        Breakpoint b{p->pe_id(), p->get_pc()};
        if (bks.count(b)) return true;
    }
    return false;
}

void show_final_results(System& sys, int N) {
    // Flush todas las caches antes de leer memoria
    for (auto& cache : sys.l1) {
        cache->flush_all();
    }
    sys.bus.flush_all();
    
    const size_t baseA_words = 0;
    const size_t baseB_words = baseA_words + static_cast<size_t>(N);
    const size_t baseS_words = baseB_words + static_cast<size_t>(N);
    
    // Calcular suma total
    double total = 0.0;
    for (unsigned p = 0; p < sys.pes.size(); ++p) {
        total += sys.mem->load64((baseS_words + p) * 8);
    }
    
    // Calcular resultado esperado
    double expected = 0.0;
    for (int i = 0; i < N; ++i) {
        double a = sys.mem->load64((baseA_words + i) * 8);
        double b = sys.mem->load64((baseB_words + i) * 8);
        expected += a * b;
    }
    
    // Mostrar solo resultados finales
    std::cout << "\n=== RESULTADOS ===" << std::endl;
    std::cout << "Producto punto calculado: " << total << std::endl;
    std::cout << "Producto punto esperado:  " << expected << std::endl;
    std::cout << "¿Correcto? " << (std::abs(total - expected) < 1e-10 ? "SI " : "NO ") << std::endl;
    
    // Opcional: mostrar sumas parciales brevemente
    std::cout << "\nSumas parciales: ";
    for (unsigned p = 0; p < sys.pes.size(); ++p) {
        double partial = sys.mem->load64((baseS_words + p) * 8);
        std::cout << "S[" << p << "]=" << partial;
        if (p < sys.pes.size() - 1) std::cout << ", ";
    }
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    unsigned num_pes = 4;
    int N = 8;  // Tamano de vectores por defecto
    
    if (argc > 1) {
        int np = 0; 
        if (to_int(argv[1], np) && np > 0) num_pes = unsigned(np);
    }
    
    if (argc > 2) {
        N = std::atoi(argv[2]);
        if (N <= 0) N = 8;
    }

    std::cout << "Inicializando sistema con " << num_pes << " PEs y N=" << N << "..." << std::endl;
    System sys(num_pes, N);  // Pasar N al constructor
    std::cout << "Stepper listo. PEs=" << num_pes << "\n";
    print_help();

    std::unordered_set<Breakpoint,BkHash> breaks;

    // Cargar programa en todos los PEs (necesitaras implementar esto)
    // Por ahora, dejamos los PEs sin programa para pruebas basicas
    
    std::string line;
    while (true) {
        std::cout << "stepper> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        auto t = split_ws(line);
        if (t.empty()) continue;

        auto cmd = t[0];
        for (auto& c: cmd) c = std::tolower(c);

        if (cmd=="help" || cmd=="h" || cmd=="?") {
            print_help();
        }
        else if (cmd=="quit" || cmd=="q" || cmd=="exit") {
            break;
        }
        else if (cmd=="regs") {
            if (t.size()==2) {
                int pe=-1; 
                if (!to_int(t[1], pe) || pe<0 || pe>=int(sys.pes.size())) { 
                    std::cout<<"pe invalido\n"; continue; 
                }
                sys.pes[pe]->dump_regs();
            } else {
                for (auto& p : sys.pes) p->dump_regs();
            }
        }
        else if (cmd=="pc") {
            if (t.size()==2) {
                int pe=-1; 
                if (!to_int(t[1], pe) || pe<0 || pe>=int(sys.pes.size())) { 
                    std::cout<<"pe invalido\n"; continue; 
                }
                std::cout << "[PE" << pe << "] PC=" << sys.pes[pe]->get_pc()
                          << " HALT=" << sys.pes[pe]->is_halted() << "\n";
            } else {
                for (auto& p : sys.pes) {
                    std::cout << "[PE" << p->pe_id() << "] PC=" << p->get_pc()
                              << " HALT=" << p->is_halted() << "\n";
                }
            }
        }
        else if (cmd=="step" || cmd=="s") {
            uint64_t n = 1;
            if (t.size()>=2) { 
                uint64_t tmp; 
                if (to_uint64(t[1], tmp)) n=tmp; 
            }
            for (uint64_t k=0; k<n; k++) {
                // round-robin: avanza 1 instruccion por PE no-halted
                bool advanced = false;
                for (auto& p : sys.pes) {
                    if (!p->is_halted()) {
                        p->step();
                        advanced = true;
                        if (hit_breakpoint(sys.pes, breaks)) break;
                    }
                }
                if (!advanced || hit_breakpoint(sys.pes, breaks)) break;
            }
        }
        else if (cmd=="stepi") {
            if (t.size()<2) { 
                std::cout<<"Uso: stepi <pe> [N]\n"; continue; 
            }
            int pe=-1; 
            if (!to_int(t[1], pe) || pe<0 || pe>=int(sys.pes.size())) { 
                std::cout<<"pe invalido\n"; continue; 
            }
            uint64_t n=1; 
            if (t.size()>=3) { 
                uint64_t tmp; 
                if (to_uint64(t[2], tmp)) n=tmp; 
            }
            for (uint64_t k=0; k<n; k++) {
                if (!sys.pes[pe]->is_halted()) {
                    sys.pes[pe]->step();
                    if (hit_breakpoint(sys.pes, breaks)) break;
                } else break;
            }
        }
        else if (cmd=="cont" || cmd=="c" || cmd=="continue") {
            int max_steps = 10000; // limite de seguridad
            int steps = 0;
            while (any_running(sys.pes) && steps < max_steps) {
                bool advanced = false;
                for (auto& p : sys.pes) {
                    if (!p->is_halted()) { 
                        p->step(); 
                        advanced = true;
                        steps++;
                    }
                }
                if (!advanced || hit_breakpoint(sys.pes, breaks)) break;
                
                // Mostrar progreso cada 1000 pasos
                if (steps % 1000 == 0) {
                    std::cout << "Continuando... pasos: " << steps << std::endl;
                }
            }
            
            if (steps >= max_steps) {
                std::cout << "ALERTA: Se alcanzo el limite de " << max_steps << " pasos" << std::endl;
            }
            
            // Mostrar resultados finales
            show_final_results(sys, N);
        }
        else if (cmd=="mem") {
            if (t.size()<2) { 
                std::cout<<"Uso: mem <addr> [count]\n"; continue; 
            }
            uint64_t addr=0; 
            if (!to_uint64(t[1], addr)) { 
                std::cout<<"addr invalida\n"; continue; 
            }
            uint64_t cnt=8; 
            if (t.size()>=3) { 
                uint64_t tmp; 
                if (to_uint64(t[2], tmp)) cnt=tmp; 
            }
            for (uint64_t i=0; i<cnt; i++) {
                double v = sys.mem->load64(addr + i*8);
                std::cout << "M[" << (addr/8 + i) << "] @0x" << std::hex << (addr+i*8) << std::dec
                          << " = " << v << "\n";
            }
        }
        else if (cmd=="cache") {
            if (t.size()<2) { 
                std::cout<<"Uso: cache <pe>\n"; continue; 
            }
            int pe=-1; 
            if (!to_int(t[1], pe) || pe<0 || pe>=int(sys.pes.size())) { 
                std::cout<<"pe invalido\n"; continue; 
            }
            sys.l1[pe]->dump_state(std::cout);
        }
        else if (cmd=="stats") {
            for (size_t i=0; i<sys.l1.size(); ++i) {
                auto& s = sys.l1[i]->stats();
                std::cout << "PE" << i << ": reads=" << s.read_ops
                          << " writes=" << s.write_ops
                          << " misses=" << s.misses
                          << " invalidations=" << s.invalidations
                          << " bus_msgs=" << s.bus_msgs << "\n";
            }
        }
        else if (cmd=="break" || cmd=="b") {
            if (t.size()<3) { 
                std::cout<<"Uso: break <pe> <pc>\n"; continue; 
            }
            int pe=-1, pc=-1;
            if (!to_int(t[1], pe) || pe<0 || pe>=int(sys.pes.size())) { 
                std::cout<<"pe invalido\n"; continue; 
            }
            if (!to_int(t[2], pc) || pc<0) { 
                std::cout<<"pc invalido\n"; continue; 
            }
            breaks.insert(Breakpoint{pe, pc});
            std::cout << "breakpoint anadido en PE" << pe << " PC=" << pc << "\n";
        }
        else if (cmd=="breaks") {
            if (breaks.empty()) {
                std::cout << "No hay breakpoints activos\n";
            } else {
                for (auto const& b : breaks) 
                    std::cout << "  PE" << b.pe_id << " PC=" << b.pc << "\n";
            }
        }
        else if (cmd=="clear") {
            if (t.size()<3) { 
                std::cout<<"Uso: clear <pe> <pc>\n"; continue; 
            }
            int pe=-1, pc=-1;
            if (!to_int(t[1], pe) || !to_int(t[2], pc)) { 
                std::cout<<"args invalidos\n"; continue; 
            }
            breaks.erase(Breakpoint{pe, pc});
            std::cout << "breakpoint eliminado\n";
        }
        else if (cmd=="status" || cmd=="st") {
            std::cout << "Estado de todos los PEs:\n";
            for (auto& p : sys.pes) {
                std::cout << "[PE" << p->pe_id() << "] PC=" << p->get_pc() 
                        << " HALT=" << p->is_halted() 
                        << " Program Size=" << /* necesitamos exponer el tamano del programa */ "?\n";
                p->dump_regs();
            }
        }
        else if (cmd=="run" || cmd=="r") {
            std::cout << "Ejecutando programa..." << std::endl;
            
            int max_steps = 10000;
            int steps = 0;
            while (any_running(sys.pes) && steps < max_steps) {
                for (auto& p : sys.pes) {
                    if (!p->is_halted()) p->step();
                }
                steps++;
            }
            
            if (steps >= max_steps) {
                std::cout << "ALERTA: Limite de pasos alcanzado" << std::endl;
            } else {
                std::cout << "Ejecucion completada en " << steps << " pasos" << std::endl;
            }
            
            show_final_results(sys, N);
        }
        else {
            std::cout << "Comando desconocido. Escriba 'help'.\n";
        }
    }

    // Flush de todas las caches antes de salir
    for (auto& c : sys.l1) c->flush_all();
    std::cout << "Saliendo del stepper...\n";
    return 0;
}