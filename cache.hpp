// cache.hpp
// C++17 — Modelo de caché privada por PE (2-way, 16 bloques, 32B) con MESI,
// write-allocate, write-back e interconnect sencillo para snooping.
// Autor: Fabian Crawford
// Compilar demo: g++ -std=c++17 -O2 -pthread cache.hpp -o cache_demo -DCACHE_DEMO

#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <atomic>
#include <chrono>

static std::mutex io_mtx;

// ------------------------------- Parámetros HW --------------------------------
namespace hw {
constexpr size_t kBlockBytes = 32;    // 32 B por línea
constexpr size_t kWays       = 2;     // 2-way
constexpr size_t kLines      = 16;    // 16 líneas totales
constexpr size_t kSets       = kLines / kWays; // 8 sets
static_assert(kSets == 8, "Deben ser 8 sets");

constexpr size_t kMemDoubles = 512;   // 512 posiciones de 64 bits
constexpr size_t kMemBytes   = kMemDoubles * sizeof(uint64_t); // 4096 B
} // namespace hw

// ------------------------------- MESI & Bus -----------------------------------
enum class MESI : uint8_t { Invalid=0, Shared=1, Exclusive=2, Modified=3 };

static inline const char* mesi_str(MESI s) {
    switch (s) {
        case MESI::Invalid:   return "I";
        case MESI::Shared:    return "S";
        case MESI::Exclusive: return "E";
        case MESI::Modified:  return "M";
    }
    return "?";
}

enum class BusCmd : uint8_t { BusRd, BusRdX, BusUpgr, Flush };

struct BusMessage {
    BusCmd   cmd;
    uint64_t addr;    // dirección física (byte address)
    int      src_pe;  // quién originó
};

// Adelanto
class Cache;

// Resumen de reacciones de snoop (agregado por el bus)
struct SnoopSummary {
    bool shared_seen = false;    // algún cache tenía copia (S/E/M)
    bool mod_seen    = false;    // algún cache estaba en M para esa línea
};

// ------------------------------- Memoria --------------------------------------
class Memory {
public:
    Memory() { bytes.resize(hw::kMemBytes, 0); }

    void writeBlockAligned(uint64_t block_addr, const std::array<uint8_t, hw::kBlockBytes>& data) {
        std::lock_guard<std::mutex> lk(m_);
        assert((block_addr % hw::kBlockBytes) == 0);
        std::memcpy(&bytes[block_addr], data.data(), hw::kBlockBytes);
    }

    void readBlockAligned(uint64_t block_addr, std::array<uint8_t, hw::kBlockBytes>& out) {
        std::lock_guard<std::mutex> lk(m_);
        assert((block_addr % hw::kBlockBytes) == 0);
        std::memcpy(out.data(), &bytes[block_addr], hw::kBlockBytes);
    }

    // Accesos escalares de 64 bits (doble precisión)
    double load64(uint64_t addr) {
        std::lock_guard<std::mutex> lk(m_);
        assert(addr + 8 <= bytes.size());
        double val;
        std::memcpy(&val, &bytes[addr], 8);
        return val;
    }
    void store64(uint64_t addr, double val) {
        std::lock_guard<std::mutex> lk(m_);
        assert(addr + 8 <= bytes.size());
        std::memcpy(&bytes[addr], &val, 8);
    }

private:
    std::vector<uint8_t> bytes;
    std::mutex m_;
};

// ------------------------------- Direccionamiento -----------------------------
struct AddrFields {
    uint64_t tag;
    uint32_t index;
    uint32_t offset;
};
struct Address {
    static constexpr uint32_t kOffBits = 5;                    // 32B -> 5 bits
    static constexpr uint32_t kIdxBits = 3;                    // 8 sets -> 3 bits
    static constexpr uint64_t kOffMask = (1u << kOffBits) - 1; // 0..31
    static constexpr uint64_t kIdxMask = (1u << kIdxBits) - 1; // 0..7

    static inline AddrFields split(uint64_t addr) {
        uint64_t off = addr & kOffMask;
        uint64_t idx = (addr >> kOffBits) & kIdxMask;
        uint64_t tag = (addr >> (kOffBits + kIdxBits));
        return {tag, static_cast<uint32_t>(idx), static_cast<uint32_t>(off)};
    }

    static inline uint64_t block_base(uint64_t addr) {
        return addr & ~static_cast<uint64_t>(kOffMask); // 32B alineado
    }
};

// ------------------------------- Interconnect ---------------------------------
class Interconnect {
public:
    void register_cache(Cache* c) {
        std::lock_guard<std::mutex> lk(m_);
        caches_.push_back(c);
    }

    // Broadcast + recolección de "shared" y writebacks implícitos
    SnoopSummary broadcast(const BusMessage& msg, Cache* origin);

private:
    std::vector<Cache*> caches_;
    std::mutex m_;
};

// ------------------------------- Caché ----------------------------------------
struct MESITransition {
    uint32_t set, way;
    MESI from, to;
    uint64_t tag;
    uint64_t addr; // dirección que provocó la transición
};

struct SnoopResponse {
    bool had_copy    = false;  // yo tenía la línea (S/E/M)
    bool wrote_back  = false;  // escribí a memoria (si estaba M y otro hace BusRd/BusRdX)
};

struct CacheLine {
    MESI state = MESI::Invalid;
    uint64_t tag = 0;
    std::array<uint8_t, hw::kBlockBytes> data{};
    bool recent = false; // bit LRU simplificado
};

struct Stats {
    uint64_t read_ops  = 0;
    uint64_t write_ops = 0;
    uint64_t misses    = 0;
    uint64_t invalidations = 0; // líneas invalidadas por snoop
    uint64_t bus_msgs  = 0;     // tráfico generado
};

class Cache {
public:
    Cache(int pe_id, Memory* mem, Interconnect* ic)
        : pe_id_(pe_id), mem_(mem), ic_(ic) {
        sets_.resize(hw::kSets);
        for (auto& set : sets_) set.fill(CacheLine{});
        if (ic_) ic_->register_cache(this);
    }

    // ---- API pública para el PE ----
    double read_double(uint64_t addr) {
        // Primera fase: probe rápido bajo lock
        uint32_t set_idx;
        uint32_t way;
        {
            std::lock_guard<std::mutex> lk(m_);
            stats_.read_ops++;
            auto f = Address::split(addr);
            auto [hit, sidx, w] = probe(f.tag, f.index);
            set_idx = sidx;
            way = w;
            if (hit) {
                // HIT: actualizar LRU/recent y devolver valor
                mark_recent(set_idx, way);
                return load_from_line(set_idx, way, f.offset);
            }
            // Si es MISS, salimos del bloque manteniendo la info necesaria
        }

        // Ya fuera del lock: emitir BusRd y recopilar snoop results
        BusMessage m{BusCmd::BusRd, addr, pe_id_};
        stats_.bus_msgs++;
        SnoopSummary sum = ic_ ? ic_->broadcast(m, this) : SnoopSummary{};

        // Segunda fase: volver a tomar lock para elegir víctima y llenar
        std::lock_guard<std::mutex> lk(m_);
        auto f2 = Address::split(addr); // recomponer campos
        // elegir índice víctima y tratar writeback si necesario
        uint32_t victim = victim_index(set_idx);
        evict_if_dirty(set_idx, victim, addr);
        fill_from_mem(addr, set_idx, victim);

        MESI new_state = sum.shared_seen ? MESI::Shared : MESI::Exclusive;
        MESI old_state = sets_[set_idx][victim].state;
        record_transition(set_idx, victim, old_state, new_state, f2.tag, addr);
        sets_[set_idx][victim].state = new_state;
        sets_[set_idx][victim].tag = f2.tag;
        mark_recent(set_idx, victim);

        return load_from_line(set_idx, victim, f2.offset);
    }



    void write_double(uint64_t addr, double value) {
        // Primera fase: probe rápido bajo lock
        uint32_t set_idx;
        uint32_t way;
        MESI cur_state = MESI::Invalid;
        {
            std::lock_guard<std::mutex> lk(m_);
            stats_.write_ops++;
            auto f = Address::split(addr);
            auto [hit, sidx, w] = probe(f.tag, f.index);
            set_idx = sidx;
            way = w;
            if (hit) {
                cur_state = sets_[set_idx][way].state;
                // Si hit en S/E/M se puede manejar sin soltar lock (solo BusUpgr necesita broadcast)
                if (cur_state == MESI::Shared) {
                    // Vamos a hacer BusUpgr — lo haremos fuera del lock
                    // marca que necesitamos BusUpgr
                } else {
                    // E o M -> podemos convertir a M y escribir localmente
                    if (cur_state == MESI::Exclusive) {
                        record_transition(set_idx, way, MESI::Exclusive, MESI::Modified, f.tag, addr);
                        sets_[set_idx][way].state = MESI::Modified;
                    }
                    store_into_line(set_idx, way, f.offset, value);
                    mark_recent(set_idx, way);
                    return;
                }
            }
            // Si no hit, salimos del lock para emitir BusRdX
        }

        // Si llegamos aquí, necesitamos emitir BusUpgr (si había S) o BusRdX (miss)
        auto f2 = Address::split(addr);
        if (cur_state == MESI::Shared) {
            // Tenemos hit en S pero necesitamos invalidar otros: BusUpgr
            BusMessage m{BusCmd::BusUpgr, addr, pe_id_};
            stats_.bus_msgs++;
            if (ic_) ic_->broadcast(m, this);
            // Tras BusUpgr, volver a tomar lock y pasar a M, escribir
            std::lock_guard<std::mutex> lk(m_);
            // encontrar way de nuevo (podría haber cambiado)
            auto [hit2, sidx2, w2] = probe(f2.tag, f2.index);
            uint32_t use_way = hit2 ? w2 : victim_index(sidx2);
            record_transition(sidx2, use_way, MESI::Shared, MESI::Modified, f2.tag, addr);
            sets_[sidx2][use_way].state = MESI::Modified;
            sets_[sidx2][use_way].tag = f2.tag;
            store_into_line(sidx2, use_way, f2.offset, value);
            mark_recent(sidx2, use_way);
            return;
        } else {
            // Miss de escritura: BusRdX (write-allocate)
            BusMessage m{BusCmd::BusRdX, addr, pe_id_};
            stats_.bus_msgs++;
            if (ic_) ic_->broadcast(m, this);

            // Ahora tomar lock y traer bloque, poner en M y escribir
            std::lock_guard<std::mutex> lk(m_);
            auto [hit3, sidx3, w3] = probe(f2.tag, f2.index);
            uint32_t victim = victim_index(sidx3);
            evict_if_dirty(sidx3, victim, addr);
            fill_from_mem(addr, sidx3, victim);
            MESI old_state = sets_[sidx3][victim].state;
            record_transition(sidx3, victim, old_state, MESI::Modified, f2.tag, addr);
            sets_[sidx3][victim].state = MESI::Modified;
            sets_[sidx3][victim].tag = f2.tag;
            store_into_line(sidx3, victim, f2.offset, value);
            mark_recent(sidx3, victim);
            return;
        }
    }



    // ---- Snoop llamado por el interconnect (no llamar desde el PE) ----
    SnoopResponse snoop(const BusMessage& msg) {
        std::lock_guard<std::mutex> lk(m_);
    {
        std::lock_guard<std::mutex> lk2(io_mtx);
        std::cout << "[PE" << pe_id_ << "] snoop got cmd=" << int(msg.cmd) << " addr=" << msg.addr << " from=" << msg.src_pe << "\n";
    }

        auto f = Address::split(msg.addr);
        auto [hit, set_idx, way] = probe(f.tag, f.index);
        SnoopResponse resp;

        if (!hit) return resp;

        auto& line = sets_[set_idx][way];
        resp.had_copy = (line.state != MESI::Invalid);

        switch (msg.cmd) {
            case BusCmd::BusRd:
                if (line.state == MESI::Modified) {
                    // Escribir a memoria y bajar a S
                    writeback_line(set_idx, way, msg.addr);
                    resp.wrote_back = true;
                    record_transition(set_idx, way, MESI::Modified, MESI::Shared, f.tag, msg.addr);
                    line.state = MESI::Shared;
                } else if (line.state == MESI::Exclusive) {
                    record_transition(set_idx, way, MESI::Exclusive, MESI::Shared, f.tag, msg.addr);
                    line.state = MESI::Shared;
                }
                // S permanece S
                break;

            case BusCmd::BusRdX:
                // Otro quiere exclusividad para escribir: invalidar y escribir back si M
                if (line.state == MESI::Modified) {
                    writeback_line(set_idx, way, msg.addr);
                    resp.wrote_back = true;
                }
                if (line.state != MESI::Invalid) {
                    stats_.invalidations++;
                    record_transition(set_idx, way, line.state, MESI::Invalid, f.tag, msg.addr);
                    line.state = MESI::Invalid;
                }
                break;

            case BusCmd::BusUpgr:
                // Otro solo quiere invalidar copias S/E (sin traer datos)
                if (line.state == MESI::Shared || line.state == MESI::Exclusive) {
                    stats_.invalidations++;
                    record_transition(set_idx, way, line.state, MESI::Invalid, f.tag, msg.addr);
                    line.state = MESI::Invalid;
                }
                break;

            case BusCmd::Flush:
                // No usado por este modelo (write-back se hace implícitamente)
                break;
        }
        return resp;
    }

    // ---- Métricas y utilidades ----
    const Stats& stats() const { return stats_; }
    const std::vector<MESITransition>& transitions() const { return trans_; }
    int pe_id() const { return pe_id_; }

    void dump_state(std::ostream& os) {
        std::lock_guard<std::mutex> lk(m_);
        os << "PE#" << pe_id_ << " Cache state (set:way tag state LRU)\n";
        for (uint32_t s = 0; s < hw::kSets; ++s) {
            for (uint32_t w = 0; w < hw::kWays; ++w) {
                const auto& l = sets_[s][w];
                os << "  " << s << ":" << w
                   << " tag=0x" << std::hex << l.tag << std::dec
                   << " state=" << mesi_str(l.state)
                   << " recent=" << (l.recent ? '1' : '0') << "\n";
            }
        }
    }

        // -- flush todas las líneas M a memoria (para asegurar visibilidad desde Memory)
    void flush_all() {
        std::lock_guard<std::mutex> lk(m_);
        for (uint32_t s = 0; s < hw::kSets; ++s) {
            for (uint32_t w = 0; w < hw::kWays; ++w) {
                auto &line = sets_[s][w];
                if (line.state == MESI::Modified) {
                    uint64_t block_addr = reconstruct_block_addr(line.tag, s);
                    mem_->writeBlockAligned(block_addr, line.data);
                    // opcional: marcar como Exclusive (es coherente con memoria)
                    line.state = MESI::Exclusive;
                }
            }
        }
    }


private:
    friend class Interconnect;

    std::tuple<bool,uint32_t,uint32_t> probe(uint64_t tag, uint32_t set_idx) const {
        for (uint32_t w=0; w<hw::kWays; ++w) {
            const auto& line = sets_[set_idx][w];
            if (line.state != MESI::Invalid && line.tag == tag) {
                return {true,set_idx,w};
            }
        }
        return {false,set_idx,0};
    }

    CacheLine& choose_victim(uint32_t set_idx) {
        // LRU bit simple: elegir el que no es "recent"; si ambos iguales, el way 0
        uint32_t w0 = 0, w1 = 1;
        if      (!sets_[set_idx][w0].recent &&  sets_[set_idx][w1].recent) return sets_[set_idx][w0];
        else if ( sets_[set_idx][w0].recent && !sets_[set_idx][w1].recent) return sets_[set_idx][w1];
        else return sets_[set_idx][0];
    }

    uint32_t victim_index(uint32_t set_idx) const {
        // mirror de choose_victim para obtener índice
        uint32_t w0 = 0, w1 = 1;
        if      (!sets_[set_idx][w0].recent &&  sets_[set_idx][w1].recent) return w0;
        else if ( sets_[set_idx][w0].recent && !sets_[set_idx][w1].recent) return w1;
        else return 0;
    }

    void mark_recent(uint32_t set_idx, uint32_t way) {
        sets_[set_idx][way].recent = true;
        sets_[set_idx][1 - way].recent = false;
    }

    void evict_if_dirty(uint32_t set_idx, uint32_t& way, uint64_t new_addr) {
        way = victim_index(set_idx);
        auto& line = sets_[set_idx][way];
        if (line.state == MESI::Modified) {
            // escribir back a memoria
            uint64_t old_block_addr = reconstruct_block_addr(line.tag, set_idx);
            mem_->writeBlockAligned(old_block_addr, line.data);
        }
        // limpiar metadatos (contenido se sobrescribe al llenar)
        line.state = MESI::Invalid;
        line.tag   = 0;
        line.recent = false;
    }

    void fill_from_mem(uint64_t addr, uint32_t set_idx, uint32_t way) {
        auto block_addr = Address::block_base(addr);
        mem_->readBlockAligned(block_addr, sets_[set_idx][way].data);
    }

    double load_from_line(uint32_t set_idx, uint32_t way, uint32_t off) const {
        double val;
        std::memcpy(&val, &sets_[set_idx][way].data[off], 8);
        return val;
    }

    void store_into_line(uint32_t set_idx, uint32_t way, uint32_t off, double v) {
        std::memcpy(&sets_[set_idx][way].data[off], &v, 8);
    }

    void writeback_line(uint32_t set_idx, uint32_t way, uint64_t addr_for_block) {
        uint64_t block_addr = Address::block_base(addr_for_block);
        mem_->writeBlockAligned(block_addr, sets_[set_idx][way].data);
    }

    uint64_t reconstruct_block_addr(uint64_t tag, uint32_t set_idx) const {
        uint64_t addr = (tag << (Address::kOffBits + Address::kIdxBits)) |
                        (static_cast<uint64_t>(set_idx) << Address::kOffBits);
        return addr; // base del bloque (offset 0)
    }

    void record_transition(uint32_t set, uint32_t way, MESI from, MESI to, uint64_t tag, uint64_t addr) {
        if (from == to) return;
        trans_.push_back(MESITransition{set,way,from,to,tag,addr});
    }

private:
    int pe_id_;
    Memory* mem_ = nullptr;
    Interconnect* ic_ = nullptr;

    std::vector<std::array<CacheLine, hw::kWays>> sets_;
    Stats stats_;
    std::vector<MESITransition> trans_;
    mutable std::mutex m_;
};

// ---------------------- Interconnect::broadcast implementación ----------------
SnoopSummary Interconnect::broadcast(const BusMessage& msg, Cache* origin) {
    std::vector<Cache*> local;
    {
        std::lock_guard<std::mutex> lk(m_);
        local = caches_;
    }
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[BUS] broadcast cmd=" << int(msg.cmd) << " addr=" << msg.addr << " src=" << msg.src_pe
                  << " targets=" << (local.size() - (origin ? 1 : 0)) << "\n";
    }
    SnoopSummary sum{};
    for (auto* c : local) {
        if (c == origin) continue;
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[BUS] sending snoop to PE" << c->pe_id() << "\n";
        }
        auto resp = c->snoop(msg);
        sum.shared_seen = sum.shared_seen || resp.had_copy;
        sum.mod_seen    = sum.mod_seen    || resp.wrote_back;
        {
            std::lock_guard<std::mutex> lk(io_mtx);
            std::cout << "[BUS] got resp from PE" << c->pe_id()
                      << " had_copy=" << resp.had_copy << " wrote_back=" << resp.wrote_back << "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lk(io_mtx);
        std::cout << "[BUS] broadcast DONE cmd=" << int(msg.cmd) << " shared=" << sum.shared_seen << " mod=" << sum.mod_seen << "\n";
    }
    return sum;
}


// ------------------------------- Mini demo opcional ---------------------------
#ifdef CACHE_DEMO
int main() {
    Memory mem;
    Interconnect bus;
    Cache c0(0, &mem, &bus);
    Cache c1(1, &mem, &bus);

    // Inicializar memoria con dos dobles contiguos (direccion 0 y 8)
    mem.store64(0,  1.5);
    mem.store64(8,  2.0);

    // Lectura en c0 (miss -> BusRd -> E)
    double a = c0.read_double(0);
    // Escritura en c1 misma línea (miss -> BusRdX en c1, invalida c0, M en c1)
    c1.write_double(8, 3.14159);

    // c0 re-lee 8 (miss -> BusRd, c1 M -> writeback + pasa a S, c0 carga S)
    double b = c0.read_double(8);

    std::cout << "a=" << a << " b=" << b << "\n";
    c0.dump_state(std::cout);
    c1.dump_state(std::cout);

    auto print_stats = [](const Cache& c){
        auto& s = c.stats();
        std::cout << "PE#" << c.pe_id()
                  << " R=" << s.read_ops
                  << " W=" << s.write_ops
                  << " Miss=" << s.misses
                  << " Inval=" << s.invalidations
                  << " Bus=" << s.bus_msgs << "\n";
    };
    print_stats(c0);
    print_stats(c1);

    // Transiciones MESI de c1
    for (auto& t : c1.transitions()) {
        std::cout << "PE#" << c1.pe_id() << " set=" << t.set
                  << " way=" << t.way
                  << " " << mesi_str(t.from) << "->" << mesi_str(t.to)
                  << " tag=0x" << std::hex << t.tag << std::dec
                  << " addr=0x" << std::hex << t.addr << std::dec << "\n";
    }
    return 0;
}
#endif

