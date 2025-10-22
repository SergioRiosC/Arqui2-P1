// cache.hpp
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

extern std::mutex io_mtx;

class SharedMemoryAdapter;
using Memory = SharedMemoryAdapter;

namespace hw {
// CONFIGURACION DEL SISTEMA DE CACHE
constexpr size_t kBlockBytes = 32;  // Tamaño de bloque de cache: 32 bytes
constexpr size_t kWays       = 2;   // Cache 2-way set associative
constexpr size_t kLines      = 16;  // Total de lineas de cache
constexpr size_t kSets       = kLines / kWays; // 8 sets (16/2)
static_assert(kSets == 8, "Deben ser 8 sets");

constexpr size_t kMemDoubles = 512; // Memoria principal: 512 doubles
constexpr size_t kMemBytes   = kMemDoubles * sizeof(uint64_t);
}

// PROTOCOLO MESI
enum class MESI : uint8_t { 
    Invalid=0,   // Linea invalida/vacia
    Shared=1,    // Compartida (lectura multiple)
    Exclusive=2, // Exclusiva (solo este PE tiene copia)
    Modified=3   // Modificada (diferente de memoria)
};

inline const char* mesi_str(MESI s) {
    switch (s) {
        case MESI::Invalid:   return "I";
        case MESI::Shared:    return "S";
        case MESI::Exclusive: return "E";
        case MESI::Modified:  return "M";
        default: return "?";
    }
}

// COMANDOS DEL BUS
enum class BusCmd : uint8_t { 
    BusRd,   // Lectura del bus
    BusRdX,  // Lectura para modificacion
    BusUpgr, // Upgrade a exclusivo
    Flush    // Escritura a memoria
};

// MENSAJE DE BUS
struct BusMessage {
    BusCmd   cmd;    // Tipo de comando
    uint64_t addr;   // Direccion accedida
    int      src_pe; // PE que origino el mensaje
};

class Cache;

// RESUMEN DE SNOOPING
struct SnoopSummary {
    bool shared_seen = false; // Alguna cache tenia copia
    bool mod_seen    = false; // Alguna cache tenia dato modificado
};

// INTERFAZ DE MEMORIA
struct IMemory {
    virtual ~IMemory() = default;
    virtual void writeBlockAligned(uint64_t block_addr, 
                                 const std::array<uint8_t, hw::kBlockBytes>& data) = 0;
    virtual void readBlockAligned(uint64_t block_addr, 
                                std::array<uint8_t, hw::kBlockBytes>& out) = 0;
    virtual double load64(uint64_t addr) = 0;
    virtual void store64(uint64_t addr, double val) = 0;
};

// CAMPOS DE DIRECCION
struct AddrFields {
    uint64_t tag;    // Parte de tag de la direccion
    uint32_t index;  // Indice del set
    uint32_t offset; // Desplazamiento dentro del bloque
};

// MANEJO DE DIRECCIONES
struct Address {
    static constexpr uint32_t kOffBits = 5;  // 5 bits para offset (32 bytes)
    static constexpr uint32_t kIdxBits = 3;  // 3 bits para indice (8 sets)
    static constexpr uint64_t kOffMask = (1u << kOffBits) - 1;
    static constexpr uint64_t kIdxMask = (1u << kIdxBits) - 1;

    static AddrFields split(uint64_t addr);     // Divide direccion en campos
    static uint64_t block_base(uint64_t addr);  // Obtiene base del bloque
};

// INTERCONEXION
class Interconnect {
public:
    void register_cache(Cache* c);           // Registrar cache en el bus
    SnoopSummary broadcast(const BusMessage& msg, Cache* origin); // Broadcast
    void flush_all();                        // Forzar write-back a memoria

private:
    std::vector<Cache*> caches_; // Lista de caches conectadas
    std::mutex m_;               // Mutex para lista de caches
    std::mutex bus_mutex_;       // Mutex para acceso al bus
};

// TRANSICION MESI
struct MESITransition {
    uint32_t set, way;  // Posicion en cache
    MESI from, to;      // Estados anterior y posterior
    uint64_t tag;       // Tag de la linea
    uint64_t addr;      // Direccion accedida
};

// RESPUESTA DE SNOOP
struct SnoopResponse {
    bool had_copy    = false;  // Esta cache tenia copia
    bool wrote_back  = false;  // Se hizo write-back
};

// LINEA DE CACHE
struct CacheLine {
    MESI state = MESI::Invalid;                    // Estado MESI
    uint64_t tag = 0;                             // Tag de la direccion
    std::array<uint8_t, hw::kBlockBytes> data{};  // Datos del bloque
    bool recent = false;                          // Bit LRU
};

// ESTADISTICAS DE CACHE
struct Stats {
    uint64_t read_ops  = 0;  // Operaciones de lectura
    uint64_t write_ops = 0;  // Operaciones de escritura
    uint64_t misses    = 0;  // Fallos de cache
    uint64_t invalidations = 0; // Invalidaciones recibidas
    uint64_t bus_msgs  = 0;  // Mensajes por bus
    uint64_t writebacks  = 0; // Write-backs a memoria
    uint64_t upgrades = 0;   // Upgrades a estado Modified
};

// CACHE L1
class Cache {
public:
    Cache(int pe_id, IMemory* mem, Interconnect* ic);
    
    // API para el PE
    double read_double(uint64_t addr);        // Leer double
    void write_double(uint64_t addr, double value); // Escribir double
    
    // Snooping para protocolo MESI
    SnoopResponse snoop(const BusMessage& msg);
    
    // Metricas y utilidades
    const Stats& stats() const { return stats_; }
    const std::vector<MESITransition>& transitions() const { return trans_; }
    int pe_id() const { return pe_id_; }
    void dump_state(std::ostream& os);        // Debug: estado de cache
    void flush_all();                         // Forzar write-back
    MESI get_state(uint32_t set_idx, uint32_t way) const;
    uint64_t get_tag(uint32_t set_idx, uint32_t way) const;
    bool get_recent(uint32_t set_idx, uint32_t way) const;

private:
    friend class Interconnect;
    
    // Metodos internos
    std::tuple<bool,uint32_t,uint32_t> probe(uint64_t tag, uint32_t set_idx) const;
    CacheLine& choose_victim(uint32_t set_idx);
    uint32_t victim_index(uint32_t set_idx) const;
    void mark_recent(uint32_t set_idx, uint32_t way);
    void evict_if_dirty(uint32_t set_idx, uint32_t& way);
    void fill_from_mem(uint64_t addr, uint32_t set_idx, uint32_t way);
    double load_from_line(uint32_t set_idx, uint32_t way, uint32_t off) const;
    void store_into_line(uint32_t set_idx, uint32_t way, uint32_t off, double v);
    void writeback_line(uint32_t set_idx, uint32_t way, uint64_t addr_for_block);
    uint64_t reconstruct_block_addr(uint64_t tag, uint32_t set_idx) const;
    void record_transition(uint32_t set, uint32_t way, MESI from, MESI to, 
                         uint64_t tag, uint64_t addr);

    // Datos miembros
    int pe_id_;         // ID del PE dueño
    IMemory* mem_ = nullptr;       // Memoria principal
    Interconnect* ic_ = nullptr;   // Bus de interconexion
    std::vector<std::array<CacheLine, hw::kWays>> sets_; // Array de sets
    Stats stats_;                   // Estadisticas
    std::vector<MESITransition> trans_; // Historial de transiciones
    mutable std::mutex m_;         // Mutex para acceso thread-safe
};