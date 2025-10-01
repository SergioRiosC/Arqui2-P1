#ifndef CACHE_H
#define CACHE_H

#include <cstddef>
#include <array>
#include <vector>
#include <mutex>
#include <cstdint>
#include <functional>
#include "interconnect.h"


// Cache como ICache para ser intercambiable con MockCache (compatibilidad).
struct CacheResponse {
    double value;
    bool hit;
    size_t busTrafficBytes;
    size_t invalidations;
    CacheResponse(): value(0.0), hit(false), busTrafficBytes(0), invalidations(0) {}
};

struct ICache {
    virtual ~ICache() = default;
    virtual CacheResponse load_double(int pe_id, size_t address) = 0;
    virtual CacheResponse store_double(int pe_id, size_t address, double value) = 0;
};

enum class MESIState { I, S, E, M };

class Cache : public ICache {
public:
    // Constructor:
    //  - memory_load/store: callbacks para leer/escribir memoria principal (function pointers)
    //  - pe_id: id del procesador al que pertenece
    //  - inter: puntero a IInterconnect (puede ser NullInterconnect si no hay bus global)
    //  - parameters are fixed by spec: 16 blocks, 2-way, 32 bytes
    Cache(int pe_id,
          std::function<double(size_t)> memory_load,
          std::function<void(size_t,double)> memory_store,
          IInterconnect* inter = nullptr);

    ~Cache() = default;

    // Implementación de ICache
    CacheResponse load_double(int pe_id, size_t address) override;
    CacheResponse store_double(int pe_id, size_t address, double value) override;

    // Mensajes que otros components (Interconnect) pueden invocar para actualizar estados
    // Cuando implementes Interconnect lo llamará para propagar coherencia
    // Devuelven true si la cache tenía el bloque (en cualquier estado).
    bool snoop_busrd(size_t block_number);   // BusRd: responder Shared / Flush si M
    bool snoop_busrdx(size_t block_number);  // BusRdX: invalidar o flush si M/S/E
    void snoop_busupgr(size_t block_number); // BusUpgr: invalidar S -> I

private:
    // Configuración fija
    static constexpr size_t BLOCK_SIZE_BYTES = 32;
    static constexpr size_t WORDS_PER_BLOCK = BLOCK_SIZE_BYTES / sizeof(double); // 4
    static constexpr size_t NUM_BLOCKS = 16;
    static constexpr size_t N_WAYS = 2;
    static constexpr size_t NUM_SETS = NUM_BLOCKS / N_WAYS; // 8

    struct Line {
        bool valid;
        MESIState state;
        size_t tag;
        bool dirty;
        std::array<double, WORDS_PER_BLOCK> data;
        uint64_t lru_counter; // para política LRU
        Line(): valid(false), state(MESIState::I), tag(0), dirty(false), lru_counter(0) {
            data.fill(0.0);
        }
    };

    // helpers
    void touch_line(Line &ln);
    size_t addr_to_block(size_t address) const;
    size_t block_to_set(size_t block_number) const;
    size_t block_to_tag(size_t block_number) const;
    void fill_line_from_memory(Line &ln, size_t block_number); // leer memoria al bloque
    void writeback_line_to_memory(Line &ln, size_t block_number); // escribir linea sucia
    Line* find_line_for_block(size_t block_number, size_t &out_set, size_t &out_way);

    // Estado interno
    int pe_id;
    IInterconnect* inter;
    std::function<double(size_t)> memory_load;
    std::function<void(size_t,double)> memory_store;
    std::array<std::array<Line, N_WAYS>, NUM_SETS> sets;
    uint64_t global_lru_counter;
    std::mutex mtx;

    // estadísticas internas
    size_t stat_traffic_bytes;
    size_t stat_invalidations;
};

#endif // CACHE_H
