#include <iostream>
#include <vector>
#include <unordered_map>
#include <thread>
#include <fstream>
#include <sstream>
#include <mutex>
#include "parser.h"
#include "instr.h"
#include "cache.h"

// ---------------- Cache / Memoria ----------------
struct CacheResponse {
    double value;
    bool hit;
    size_t busTrafficBytes;
    CacheResponse(): value(0.0), hit(false), busTrafficBytes(0) {}
};

class Memory {
public:
    Memory(size_t words): mem(words,0.0) {}
    double load(size_t addr) {
        std::lock_guard<std::mutex> lk(mtx);
        return mem[addr];
    }
    void store(size_t addr,double v) {
        std::lock_guard<std::mutex> lk(mtx);
        mem[addr]=v;
    }
private:
    std::vector<double> mem;
    std::mutex mtx;
};

class MockCache {
public:
    MockCache(Memory* mem,int pe_id): mem(mem), pe_id(pe_id) {}
    CacheResponse load_double(int requester_pe_id, size_t address) {
        CacheResponse r;
        auto it=local_store.find(address);
        if(it!=local_store.end()){
            r.hit=true; r.value=it->second;
        } else {
            r.hit=false;
            r.value=mem->load(address);
            local_store[address]=r.value;
            r.busTrafficBytes=sizeof(double);
        }
        return r;
    }
    CacheResponse store_double(int requester_pe_id, size_t address, double value) {
        CacheResponse r;
        mem->store(address,value);
        local_store[address]=value;
        r.busTrafficBytes=sizeof(double);
        return r;
    }
private:
    Memory* mem;
    int pe_id;
    std::unordered_map<size_t,double> local_store;
};

// ---------------- PE ----------------
class PE {
public:
    PE(int id, MockCache* cache): id(id), cache(cache), pc(0), halt_flag(false) {
        for(int i=0;i<8;i++) regs_raw[i]=0;
    }

    void load_program(const std::vector<Instr>& prog,
                      const std::unordered_map<std::string,size_t>& labels){
        program=prog; label_map=labels; pc=0; halt_flag=false;
    }

    void run(){
        while(!halt_flag && pc<(int)program.size()){
            step();
        }
    }

    double regs_raw[8];

    double get_reg_double(int r) const { return regs_raw[r]; }
    void set_reg_double(int r, double v) { regs_raw[r] = v; }

    int get_reg_int(int r) const { return static_cast<int>(regs_raw[r]); }
    void set_reg_int(int r, int v) { regs_raw[r] = static_cast<double>(v); }

    struct {
        uint64_t cache_misses=0,cache_hits=0,loads=0,stores=0,bus_traffic=0;
    } stats;

private:
    int id;
    MockCache* cache;
    int pc;
    bool halt_flag;
    std::vector<Instr> program;
    std::unordered_map<std::string,size_t> label_map;

    void step(){
        Instr I=program[pc];
        switch(I.op){
            case OpCode::LOAD: exec_load(I); break;
            case OpCode::STORE: exec_store(I); break;
            case OpCode::FMUL: exec_fmul(I); break;
            case OpCode::FADD: exec_fadd(I); break;
            case OpCode::INC: exec_inc(I); break;
            case OpCode::DEC: exec_dec(I); break;
            case OpCode::JNZ: exec_jnz(I); break;
            case OpCode::HALT: halt_flag=true; break;
            default: break;
        }
        pc++;
    }

    void exec_load(const Instr& I){
        size_t addr = I.addr_is_reg ? get_reg_int(I.ra) : I.address;
        auto r = cache->load_double(id, addr);
        set_reg_double(I.rd, r.value);
        stats.loads++; r.hit?stats.cache_hits++:stats.cache_misses++; stats.bus_traffic+=r.busTrafficBytes;
    }

    void exec_store(const Instr& I){
        size_t addr = I.addr_is_reg ? get_reg_int(I.ra) : I.address;
        auto r = cache->store_double(id, addr, get_reg_double(I.rd));
        stats.stores++; r.hit?stats.cache_hits++:stats.cache_misses++; stats.bus_traffic+=r.busTrafficBytes;
    }

    void exec_fmul(const Instr& I){
        set_reg_double(I.rd, get_reg_double(I.ra) * get_reg_double(I.rb));
    }
    void exec_fadd(const Instr& I){
        set_reg_double(I.rd, get_reg_double(I.ra) + get_reg_double(I.rb));
    }
    void exec_inc(const Instr& I){ set_reg_int(I.rd,get_reg_int(I.rd)+1); }
    void exec_dec(const Instr& I){ set_reg_int(I.rd,get_reg_int(I.rd)-1); }
    void exec_jnz(const Instr& I){
        if(get_reg_int(I.rd) != 0) {
            auto it = label_map.find(I.label);
            if(it != label_map.end())
                pc = int(it->second) - 1;
        }
    }
};

// ---------------- Main ----------------
int main() {
    constexpr size_t MEM_WORDS = 512;
    Memory mem(MEM_WORDS);

    size_t baseA = 0, baseB = 100, baseS = 300;
    const int N = 8, P = 4;
    int seg = N / P;

    // Inicializar A y B
    for (int i = 0; i < N; i++) {
        mem.store(baseA + i, double(i + 1));
        mem.store(baseB + i, double((i + 1) * 2));
    }

    // Crear caches y PEs
    std::vector<std::unique_ptr<MockCache>> caches;
    std::vector<std::unique_ptr<PE>> pes;
    for (int i = 0; i < P; i++) {
        auto mem_load = [&mem](size_t addr){ return mem.load(addr); };
        auto mem_store = [&mem](size_t addr, double v){ mem.store(addr, v); };
        caches.push_back(std::make_unique<Cache>(i, mem_load, mem_store, inter_ptr));

        pes.push_back(std::make_unique<PE>(i, caches[i].get()));
    }

    // Cargar programa ASM
    std::ifstream fin("dotprod.asm");
    if (!fin) {
        std::cerr << "Error: no se pudo abrir dotprod.asm\n";
        return 1;
    }
    std::stringstream buffer; buffer << fin.rdbuf();
    std::vector<Instr> prog;
    std::unordered_map<std::string, size_t> labels;
    parse_asm(buffer.str(), prog, labels);

    // Inicializar registros
    for (int p = 0; p < P; p++) {
        pes[p]->load_program(prog, labels);
        pes[p]->set_reg_int(0, baseA + p * seg);
        pes[p]->set_reg_int(1, baseB + p * seg);
        pes[p]->set_reg_int(2, baseS + p);
        pes[p]->set_reg_int(3, seg);
        pes[p]->set_reg_double(4, 0.0);
    }

    // Ejecutar
    std::vector<std::thread> threads;
    for (int p = 0; p < P; p++) {
        threads.emplace_back([&pes, p]() { pes[p]->run(); });
    }
    for (auto &t : threads) t.join();

    // Resultados
    for (int p = 0; p < P; p++) {
        std::cout << "PE" << p << " sum stored at M[" << (baseS + p)
                  << "] = " << mem.load(baseS + p) << "\n";
    }

    double total = 0;
    for (int p = 0; p < P; p++) total += mem.load(baseS + p);

    double expected = 0;
    for (int i = 0; i < N; i++) expected += mem.load(baseA + i) * mem.load(baseB + i);

    std::cout << "\nProducto punto (reducción final) = " << total << "\n";
    std::cout << "Producto punto (esperado secuencial) = " << expected << "\n\n";

    std::cout << "Estadísticas por PE:\n";
    for (int p = 0; p < P; p++) {
        auto &s = pes[p]->stats;
        std::cout << "PE" << p << ": loads=" << s.loads
                  << " stores=" << s.stores
                  << " hits=" << s.cache_hits
                  << " misses=" << s.cache_misses
                  << " traffic(bytes)=" << s.bus_traffic
                  << "\n";
    }

    return 0;
}
