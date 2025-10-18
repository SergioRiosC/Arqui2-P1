#include "cache.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <iostream>
#include <iomanip>

static inline void cdbg(int pe, const std::string &msg) {
    std::cout << "[CACHE PE" << pe << "] " << msg << std::endl;
}

// ---------------- helpers privados ----------------

Cache::Cache(int pe_id_,
             std::function<double(size_t)> memory_load_,
             std::function<void(size_t,double)> memory_store_,
             IInterconnect* inter_)
    : pe_id(pe_id_), inter(inter_), memory_load(memory_load_), memory_store(memory_store_),
      global_lru_counter(1), stat_traffic_bytes(0), stat_invalidations(0)
{
    // sets y lines inicializados por constructor de Line
}

void Cache::touch_line(Line &ln) {
    ln.lru_counter = ++global_lru_counter;
}

size_t Cache::addr_to_block(size_t address) const {
    // address es índice de palabra (double). Convertimos a número de bloque
    return address / WORDS_PER_BLOCK;
}

size_t Cache::block_to_set(size_t block_number) const {
    return block_number % NUM_SETS;
}

size_t Cache::block_to_tag(size_t block_number) const {
    return block_number / NUM_SETS;
}

// Busca línea; si no existe devuelve nullptr. out_set = set index; out_way definido sólo si se encuentra.
Cache::Line* Cache::find_line_for_block(size_t block_number, size_t &out_set, size_t &out_way) {
    out_set = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    for (size_t w = 0; w < N_WAYS; ++w) {
        Line &ln = sets[out_set][w];
        if (ln.valid && ln.tag == tag) {
            out_way = w;
            return &ln;
        }
    }
    return nullptr;
}

void Cache::fill_line_from_memory(Line &ln, size_t block_number) {
    // Lee WORDS_PER_BLOCK palabras desde memory_load (no contabiliza tráfico aquí,
    // lo hace el llamador, para mayor claridad).
    size_t base_addr = block_number * WORDS_PER_BLOCK;
    for (size_t i = 0; i < WORDS_PER_BLOCK; ++i) {
        ln.data[i] = memory_load(base_addr + i);
    }
    ln.valid = true;
    ln.dirty = false;
    // no incrementamos stat_traffic_bytes aquí: el llamador decide cuánto sumar
}

void Cache::writeback_line_to_memory(Line &ln, size_t block_number) {
    if (!ln.valid || !ln.dirty) return;
    size_t base_addr = block_number * WORDS_PER_BLOCK;
    for (size_t i = 0; i < WORDS_PER_BLOCK; ++i) {
        memory_store(base_addr + i, ln.data[i]);
    }
    ln.dirty = false;
    // contamos tráfico del writeback (bloque entero)
    stat_traffic_bytes += BLOCK_SIZE_BYTES;
    if (inter) {
        inter->broadcast_flush(pe_id, block_number, ln.data.data(), WORDS_PER_BLOCK);
    }
}

// ---------------- snoop handlers (llamados por el interconnect) ----------------

bool Cache::snoop_busrd(size_t block_number) {
    std::lock_guard<std::mutex> lk(mtx);
    size_t set = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    for (size_t w=0; w<N_WAYS; ++w) {
        Line &ln = sets[set][w];
        if (ln.valid && ln.tag == tag) {
            if (ln.state == MESIState::M) {
                // flush dirty data to memory and move to S
                writeback_line_to_memory(ln, block_number);
                ln.state = MESIState::S;
            } else if (ln.state == MESIState::E) {
                ln.state = MESIState::S;
            }
            return true;
        }
    }
    return false;
}

bool Cache::snoop_busrdx(size_t block_number) {
    std::lock_guard<std::mutex> lk(mtx);
    size_t set = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    bool had = false;
    for (size_t w=0; w<N_WAYS; ++w) {
        Line &ln = sets[set][w];
        if (ln.valid && ln.tag == tag) {
            had = true;
            if (ln.state == MESIState::M) {
                writeback_line_to_memory(ln, block_number);
            }
            // invalidar
            ln.state = MESIState::I;
            ln.valid = false;
        }
    }
    if (had) stat_invalidations++;
    return had;
}

void Cache::snoop_busupgr(size_t block_number) {
    std::lock_guard<std::mutex> lk(mtx);
    size_t set = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    for (size_t w=0; w<N_WAYS; ++w) {
        Line &ln = sets[set][w];
        if (ln.valid && ln.tag == tag) {
            if (ln.state == MESIState::S) {
                ln.state = MESIState::I;
                ln.valid = false;
                stat_invalidations++;
            }
        }
    }
}

// ---------------- operaciones principales (load/store) ----------------

CacheResponse Cache::load_double(int pe_id_call, size_t address) {
    CacheResponse resp;
    std::lock_guard<std::mutex> lk(mtx);

    size_t block_number = addr_to_block(address);
    size_t set_idx = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    cdbg(pe_id, "LOAD addr=" + std::to_string(address) +
                " block=" + std::to_string(block_number) +
                " set=" + std::to_string(set_idx) +
                " tag=" + std::to_string(tag));
    size_t way = 0;
    Line* ln = find_line_for_block(block_number, set_idx, way);

    if (ln && ln->valid && ln->state != MESIState::I) {
        // HIT (S, E o M)
        resp.hit = true;
        size_t offset = address % WORDS_PER_BLOCK;
        resp.value = ln->data[offset];
        cdbg(pe_id, "HIT way=" + std::to_string(way) + " state=" + std::to_string((int)ln->state) +
                   " val=" + std::to_string(resp.value));
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        return resp;
    }
    cdbg(pe_id, "MISS -> busrd block=" + std::to_string(block_number));

    // MISS: traer bloque
    resp.hit = false;

    bool suppliedByOthers = false;
    if (inter) {
        suppliedByOthers = inter->broadcast_busrd(pe_id, block_number);
    }

    // elegir víctima vía LRU
    size_t victim_way = 0;
    uint64_t min_lru = std::numeric_limits<uint64_t>::max();
    for (size_t w = 0; w < N_WAYS; ++w) {
        Line &cand = sets[set_idx][w];
        if (!cand.valid) { victim_way = w; min_lru = 0; break; }
        if (cand.lru_counter < min_lru) { min_lru = cand.lru_counter; victim_way = w; }
    }
    Line &victim = sets[set_idx][victim_way];

    // si víctima sucia -> writeback
    if (victim.valid && victim.dirty) {
        size_t victim_block = victim.tag * NUM_SETS + set_idx;
        writeback_line_to_memory(victim, victim_block);
    }

    // llenar victor con bloque nuevo
    victim.tag = tag;
    victim.valid = true;

    // Traer datos desde memoria (o suponer flush hecho por interconnect)
    fill_line_from_memory(victim, block_number);
    cdbg(pe_id, "After fill: victim_tag=" + std::to_string(victim.tag) +
               " data0=" + std::to_string(victim.data[0]));

    // Contabilizar tráfico por traer el bloque
    stat_traffic_bytes += BLOCK_SIZE_BYTES;
    resp.busTrafficBytes += BLOCK_SIZE_BYTES;

    // Estado: S si otros tenían, E si nadie más
    victim.state = (suppliedByOthers ? MESIState::S : MESIState::E);
    victim.dirty = false;
    touch_line(victim);

    size_t offset = address % WORDS_PER_BLOCK;
    resp.value = victim.data[offset];
    cdbg(pe_id, "Returning value=" + std::to_string(resp.value));
    return resp;
}

CacheResponse Cache::store_double(int pe_id_call, size_t address, double value) {
    CacheResponse resp;
    std::lock_guard<std::mutex> lk(mtx);

    size_t block_number = addr_to_block(address);
    size_t set_idx = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    cdbg(pe_id, "STORE addr=" + std::to_string(address) +
                " block=" + std::to_string(block_number) +
                " set=" + std::to_string(set_idx) +
                " tag=" + std::to_string(tag));
    size_t way = 0;
    Line* ln = find_line_for_block(block_number, set_idx, way);

    if (ln && ln->valid && ln->state == MESIState::M) {
        // HIT en M -> escribir
        resp.hit = true;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        memory_store(address, value);   // <-- write-through
        cdbg(pe_id, "HIT way=" + std::to_string(way) + " state=" + std::to_string((int)ln->state) +
                   " val=" + std::to_string(value));
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        return resp;
    }
    cdbg(pe_id, "MISS -> busrd block=" + std::to_string(block_number));

    if (ln && ln->valid && ln->state == MESIState::E) {
        // HIT en E -> pasar a M y escribir
        resp.hit = true;
        ln->state = MESIState::M;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        memory_store(address, value);   // <-- write-through
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        return resp;
    }

    if (ln && ln->valid && ln->state == MESIState::S) {
        // HIT en S -> pedir BusUpgr y pasar a M
        resp.hit = true;
        if (inter) inter->broadcast_busupgr(pe_id, block_number);
        ln->state = MESIState::M;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        memory_store(address, value);   // <-- write-through
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        resp.invalidations = 1;
        return resp;
    }

    // MISS en store -> write-allocate: BusRdX y traer bloque
    resp.hit = false;
    if (inter) {
        inter->broadcast_busrdx(pe_id, block_number);
    }

    // elegir víctima y writeback si hace falta
    size_t victim_way = 0;
    uint64_t min_lru = std::numeric_limits<uint64_t>::max();
    for (size_t w = 0; w < N_WAYS; ++w) {
        Line &cand = sets[set_idx][w];
        if (!cand.valid) { victim_way = w; min_lru = 0; break; }
        if (cand.lru_counter < min_lru) { min_lru = cand.lru_counter; victim_way = w; }
    }
    Line &victim = sets[set_idx][victim_way];

    if (victim.valid && victim.dirty) {
        size_t victim_block = victim.tag * NUM_SETS + set_idx;
        writeback_line_to_memory(victim, victim_block);
    }

    // llenar bloque desde memoria
    victim.tag = tag;
    victim.valid = true;
    fill_line_from_memory(victim, block_number);
    cdbg(pe_id, "After fill: victim_tag=" + std::to_string(victim.tag) +
               " data0=" + std::to_string(victim.data[0]));

    // contabilizar tráfico
    stat_traffic_bytes += BLOCK_SIZE_BYTES;
    resp.busTrafficBytes += BLOCK_SIZE_BYTES;

    // nos quedamos con el bloque en M y escribimos
    victim.state = MESIState::M;
    victim.dirty = true;
    size_t offset = address % WORDS_PER_BLOCK;
    victim.data[offset] = value;
    memory_store(address, value);   // <-- write-through
    touch_line(victim);
    cdbg(pe_id, "Returning value=" + std::to_string(value));

    return resp;
}
