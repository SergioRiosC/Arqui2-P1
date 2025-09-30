#include <iostream>
#include <vector>
#include <array>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <fstream>
#include <sstream>
#include <cstring>

// ---------------- ISA ----------------
enum class OpCode { LOAD, STORE, FMUL, FADD, INC, DEC, JNZ, NOP, HALT };

struct Instr {
    OpCode op;
    int rd, ra, rb;
    bool addr_is_reg;
    size_t address;
    std::string label;
    Instr(OpCode o=OpCode::NOP): op(o), rd(0), ra(-1), rb(-1),
        addr_is_reg(false), address(0), label("") {}
};

struct CacheResponse {
    double value;
    bool hit;
    size_t busTrafficBytes;
    size_t invalidations;
    CacheResponse(): value(0.0), hit(false), busTrafficBytes(0), invalidations(0) {}
};

struct ICache {
    virtual ~ICache()=default;
    virtual CacheResponse load_double(int pe_id, size_t address)=0;
    virtual CacheResponse store_double(int pe_id, size_t address, double value)=0;
};

// ---------------- Memory & MockCache ----------------
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
    void dump_range(size_t start,size_t n) {
        for(size_t i=0;i<n;i++) {
            std::cout<<"M["<<start+i<<"]="<<mem[start+i]<<"\n";
        }
    }
private:
    std::vector<double> mem;
    std::mutex mtx;
};

class MockCache: public ICache {
public:
    MockCache(Memory* mem,int pe_id): mem(mem), pe_id(pe_id) {}
    CacheResponse load_double(int requester_pe_id, size_t address) override {
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
    CacheResponse store_double(int requester_pe_id, size_t address, double value) override {
        CacheResponse r;
        auto it=local_store.find(address);
        if(it!=local_store.end()){
            r.hit=true; it->second=value; mem->store(address,value);
            r.busTrafficBytes=sizeof(double);
        } else {
            r.hit=false;
            mem->store(address,value);
            local_store[address]=value;
            r.busTrafficBytes=sizeof(double);
        }
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
    PE(int id, ICache* cache): id(id), cache(cache), pc(0), halt_flag(false) {
        for(int i=0;i<8;i++) regs_raw[i]=0;
    }

    void load_program(const std::vector<Instr>& prog,const std::unordered_map<std::string,size_t>& labels){
        program=prog; label_map=labels; pc=0; halt_flag=false;
    }

    void run(){
        while(!halt_flag && pc<(int)program.size()){
            step();
        }
    }
    double regs_raw[8];  // todos los registros como double

    double get_reg_double(int r) const { return regs_raw[r]; }
    void set_reg_double(int r, double v) { regs_raw[r] = v; }

    int get_reg_int(int r) const { return static_cast<int>(regs_raw[r]); }
    void set_reg_int(int r, int v) { regs_raw[r] = static_cast<double>(v); }

    struct {
        uint64_t cache_misses=0,cache_hits=0,loads=0,stores=0,invalidations=0,bus_traffic=0;
    } stats;

    //uint64_t regs_raw[8];

    //int64_t get_reg_int(int r) const { return (int64_t)regs_raw[r]; }
    //void set_reg_int(int r,int64_t v){ regs_raw[r]=v; }

private:
    int id;
    ICache* cache;
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
        size_t addr = I.addr_is_reg ? get_reg_int(I.rb) : I.address;
        auto r = cache->store_double(id, addr, get_reg_double(I.rd));
        stats.stores++; r.hit?stats.cache_hits++:stats.cache_misses++; stats.bus_traffic+=r.busTrafficBytes;
    }

    void exec_fmul(const Instr& I){
        double a=get_reg_double(I.ra),b=get_reg_double(I.rb); set_reg_double(I.rd,a*b);
    }
    void exec_fadd(const Instr& I){
        double a=get_reg_double(I.ra),b=get_reg_double(I.rb); set_reg_double(I.rd,a+b);
    }
    void exec_inc(const Instr& I){ set_reg_int(I.rd,get_reg_int(I.rd)+1); }
    void exec_dec(const Instr& I){ set_reg_int(I.rd,get_reg_int(I.rd)-1); }
    void exec_jnz(const Instr& I){
        // validar registro
        int reg_idx = I.rd;
        if (reg_idx < 0 || reg_idx >= 8) {
            // registro inválido: no hacemos salto
            return;
        }
        if (get_reg_int(reg_idx) != 0) {
            auto it = label_map.find(I.label);
            if (it != label_map.end()) {
                pc = int(it->second) - 1; // -1 porque pc++ ocurre después
            } else {
                // etiqueta no encontrada -> no saltar (o loggear)
                // std::cerr << "JNZ: etiqueta '" << I.label << "' no encontrada\n";
            }
        }
    }


};

// ---------------- Assembler ----------------
static inline std::string trim(const std::string&s){
    size_t a=s.find_first_not_of(" \t\r\n"); if(a==std::string::npos) return "";
    size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1);
}
bool is_register_token(const std::string &tok,int &reg_out){
    if(tok.size()>=2&&(tok[0]=='R'||tok[0]=='r')){
        try{int r=std::stoi(tok.substr(1)); if(r>=0&&r<8){reg_out=r;return true;}}catch(...){}
    } return false;
}
bool parse_number(const std::string&tok,size_t&out){
    try{long long v=std::stoll(tok); if(v<0) return false; out=(size_t)v; return true;}catch(...){return false;}
}

Instr make_instr_from_tokens(const std::vector<std::string>&toks){
    Instr I; if(toks.empty()) return I; std::string op=toks[0]; for(auto&c:op)c=toupper(c);
    if(op=="LOAD"){ I.op=OpCode::LOAD; int rd; std::string rdtok=toks[1]; if(rdtok.back()==',') rdtok.pop_back();
        if(is_register_token(rdtok,rd)) I.rd=rd; std::string opnd=toks[2]; if(toks.size()>3) for(size_t i=3;i<toks.size();i++) opnd+=toks[i];
        if(opnd.front()=='['&&opnd.back()==']'){int r; if(is_register_token(opnd.substr(1,opnd.size()-2),r)){I.addr_is_reg=true; I.ra=r;}}
        else {size_t addr; if(parse_number(opnd,addr)){I.addr_is_reg=false; I.address=addr;}} }
    else if(op=="STORE"){ I.op=OpCode::STORE; int rd; std::string rdtok=toks[1]; if(rdtok.back()==',') rdtok.pop_back();
        if(is_register_token(rdtok,rd)) I.rd=rd; std::string opnd=toks[2]; if(toks.size()>3) for(size_t i=3;i<toks.size();i++) opnd+=toks[i];
        if(opnd.front()=='['&&opnd.back()==']'){int r; if(is_register_token(opnd.substr(1,opnd.size()-2),r)){I.addr_is_reg=true; I.rb=r;}}
        else {size_t addr; if(parse_number(opnd,addr)){I.addr_is_reg=false; I.address=addr;}} }
    else if(op=="FMUL"||op=="FADD"){ I.op=(op=="FMUL"?OpCode::FMUL:OpCode::FADD); int rd,ra,rb; 
        std::string r1=toks[1]; if(r1.back()==',') r1.pop_back(); std::string r2=toks[2]; if(r2.back()==',') r2.pop_back();
        std::string r3=toks[3]; if(is_register_token(r1,rd)&&is_register_token(r2,ra)&&is_register_token(r3,rb)){I.rd=rd;I.ra=ra;I.rb=rb;} }
    else if(op=="INC"||op=="DEC"){ I.op=(op=="INC"?OpCode::INC:OpCode::DEC); int rd; if(is_register_token(toks[1],rd)) I.rd=rd; }
    else if (op == "JNZ") {
        I.op = OpCode::JNZ;
        // soporta "JNZ LABEL"  o "JNZ Rn, LABEL" o "JNZ Rn LABEL"
        if (toks.size() >= 2) {
            int reg = -1;
            // si el primer token es un registro
            if (is_register_token(toks[1], reg)) {
                I.rd = reg;
                // label puede estar en toks[2] o toks[1] tras coma
                if (toks.size() >= 3) {
                    I.label = toks[2];
                } else {
                    I.label = ""; // etiqueta faltante -> será detectado luego
                }
            } else {
                // forma "JNZ LABEL" -> usamos registro implícito (I.rd ya 0 por defecto)
                I.label = toks[1];
            }
        }
    }

    else if(op=="HALT"){ I.op=OpCode::HALT; }
    else I.op=OpCode::NOP; return I;
}

void parse_asm(const std::string&asm_text,std::vector<Instr>&out_program,std::unordered_map<std::string,size_t>&out_label_map){
    out_program.clear(); out_label_map.clear(); std::istringstream ss(asm_text); std::string line; std::vector<std::string> cleaned;
    while(std::getline(ss,line)){ size_t pos=line.find("#"); if(pos!=std::string::npos) line=line.substr(0,pos);
        line=trim(line); if(line.empty()) continue; if(line.back()==':'){ out_label_map[line.substr(0,line.size()-1)]=cleaned.size(); continue; }
        cleaned.push_back(line);}
    for(auto&ln:cleaned){ std::vector<std::string> toks; std::string cur; for(char c:ln){ if(c==' '||c=='\t'||c==','){ if(!cur.empty()){toks.push_back(cur);cur.clear();}} else cur.push_back(c);} if(!cur.empty()) toks.push_back(cur);
        out_program.push_back(make_instr_from_tokens(toks)); }
}

int main() {
    constexpr size_t MEM_WORDS = 512;
    Memory mem(MEM_WORDS);

    size_t baseA = 0, baseB = 100, baseS = 300;
    const int N = 8, P = 4; 
    int seg = N / P;

    // Inicializar vectores A y B en memoria
    for (int i = 0; i < N; i++) {
        mem.store(baseA + i, double(i + 1));      // A[i] = 1..N
        mem.store(baseB + i, double((i + 1) * 2)); // B[i] = 2,4,6,...
    }

    // Crear caches y PEs
    std::vector<std::unique_ptr<MockCache>> caches;
    std::vector<std::unique_ptr<PE>> pes;
    for (int i = 0; i < P; i++) {
        caches.push_back(std::make_unique<MockCache>(&mem, i));
        pes.push_back(std::make_unique<PE>(i, caches[i].get()));
    }

    // Leer el mismo programa ASM genérico
    std::ifstream fin("dotprod.asm");
    if (!fin) {
        std::cerr << "Error: no se pudo abrir dotprod.asm\n";
        return 1;
    }
    std::stringstream buffer; buffer << fin.rdbuf();
    std::vector<Instr> prog;
    std::unordered_map<std::string, size_t> labels;
    parse_asm(buffer.str(), prog, labels);

    // Cargar el programa en cada PE e inicializar registros
    for (int p = 0; p < P; p++) {
        pes[p]->load_program(prog, labels);
        pes[p]->set_reg_int(0, baseA + p * seg);   // R0 = inicio segmento A
        pes[p]->set_reg_int(1, baseB + p * seg);   // R1 = inicio segmento B
        pes[p]->set_reg_int(2, baseS + p);         // R2 = dirección suma parcial
        pes[p]->set_reg_int(3, seg);               // R3 = contador iteraciones
        pes[p]->set_reg_double(4, 0.0);            // R4 = acumulador inicial
    }

    // Ejecutar PEs en paralelo
    std::vector<std::thread> threads;
    for (int p = 0; p < P; p++) {
        threads.emplace_back([&pes, p]() { pes[p]->run(); });
    }
    for (auto &t : threads) t.join();

    // Mostrar sumas parciales
    for (int p = 0; p < P; p++) {
        std::cout << "PE" << p << " sum stored at M[" << (baseS + p) 
                  << "] = " << mem.load(baseS + p) << "\n";
    }

    // Calcular producto punto final
    double total = 0;
    for (int p = 0; p < P; p++) total += mem.load(baseS + p);
    
    double expected = 0;
    for (int i = 0; i < N; i++) expected += mem.load(baseA + i) * mem.load(baseB + i);

    std::cout << "\nProducto punto (reducción final) = " << total << "\n";
    std::cout << "Producto punto (esperado secuencial) = " << expected << "\n\n";

    // Estadísticas
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

