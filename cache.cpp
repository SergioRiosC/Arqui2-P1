#include "cache.h"
#include <algorithm>
#include <iostream>

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
    // Lee WORDS_PER_BLOCK palabras desde memory_load
    size_t base_addr = block_number * WORDS_PER_BLOCK;
    for (size_t i = 0; i < WORDS_PER_BLOCK; ++i) {
        ln.data[i] = memory_load(base_addr + i);
    }
    ln.valid = true;
    ln.dirty = false;
    // estadistica: tráfico del bus al traer bloque
    stat_traffic_bytes += BLOCK_SIZE_BYTES;
}

void Cache::writeback_line_to_memory(Line &ln, size_t block_number) {
    if (!ln.valid || !ln.dirty) return;
    size_t base_addr = block_number * WORDS_PER_BLOCK;
    for (size_t i = 0; i < WORDS_PER_BLOCK; ++i) {
        memory_store(base_addr + i, ln.data[i]);
    }
    ln.dirty = false;
    stat_traffic_bytes += BLOCK_SIZE_BYTES;
    // notificar interconnect si se implementa
    if (inter) {
        inter->broadcast_flush(pe_id, block_number, ln.data.data(), WORDS_PER_BLOCK);
    }
}

// ---------------- snoop handlers (llamados por el interconnect) ----------------

// Devuelve true si tenía la línea (en cualquier estado) — esto permite que el interconnect
// sepa si alguien la tenía y actúe (ej. forzar flush).
bool Cache::snoop_busrd(size_t block_number) {
    std::lock_guard<std::mutex> lk(mtx);
    size_t set = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    for (size_t w=0; w<N_WAYS; ++w) {
        Line &ln = sets[set][w];
        if (ln.valid && ln.tag == tag) {
            // Si estaba M -> debe escribir (flush) y convertirse en S
            if (ln.state == MESIState::M) {
                writeback_line_to_memory(ln, block_number);
                ln.state = MESIState::S;
            } else if (ln.state == MESIState::E) {
                // si alguien pide BusRd y estaba exclusivo, pasa a shared
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
            // Si estaba Shared -> invalidar
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
    size_t way = 0;
    Line* ln = find_line_for_block(block_number, set_idx, way);

    if (ln && ln->valid && ln->state != MESIState::I) {
        // HIT (S, E o M)
        resp.hit = true;
        size_t offset = address % WORDS_PER_BLOCK;
        resp.value = ln->data[offset];
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        return resp;
    }

    // MISS: need to bring block
    resp.hit = false;

    // Ask interconnect whether others have it (BusRd)
    bool suppliedByOthers = false;
    if (inter) {
        suppliedByOthers = inter->broadcast_busrd(pe_id, block_number);
    } else {
        // NullInterconnect: no one supplies
        suppliedByOthers = false;
    }

    // Choose victim via LRU within set
    size_t victim_way = 0;
    uint64_t min_lru = UINT64_MAX;
    for (size_t w = 0; w < N_WAYS; ++w) {
        Line &cand = sets[set_idx][w];
        if (!cand.valid) { victim_way = w; min_lru = 0; break; }
        if (cand.lru_counter < min_lru) { min_lru = cand.lru_counter; victim_way = w; }
    }
    Line &victim = sets[set_idx][victim_way];

    // If victim dirty, write back
    if (victim.valid && victim.dirty) {
        size_t victim_block = victim.tag * NUM_SETS + set_idx;
        writeback_line_to_memory(victim, victim_block);
    }

    // Fill victim with new block from memory (or from other cache via interconnect)
    size_t new_tag = tag;
    victim.tag = new_tag;
    victim.valid = true;
    // If someone supplied (interconnect), we assume memory already updated/flush done.
    fill_line_from_memory(victim, block_number);

    // Set state: if someone else had it -> Shared, else Exclusive
    if (suppliedByOthers) {
        victim.state = MESIState::S;
    } else {
        victim.state = MESIState::E;
    }
    victim.dirty = false;
    touch_line(victim);

    size_t offset = address % WORDS_PER_BLOCK;
    resp.value = victim.data[offset];
    resp.busTrafficBytes += BLOCK_SIZE_BYTES; // already accounted in fill_line_from_memory too
    return resp;
}

CacheResponse Cache::store_double(int pe_id_call, size_t address, double value) {
    CacheResponse resp;
    std::lock_guard<std::mutex> lk(mtx);

    size_t block_number = addr_to_block(address);
    size_t set_idx = block_to_set(block_number);
    size_t tag = block_to_tag(block_number);
    size_t way = 0;
    Line* ln = find_line_for_block(block_number, set_idx, way);

    if (ln && ln->valid && ln->state == MESIState::M) {
        // HIT in M -> write and mark dirty
        resp.hit = true;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        return resp;
    }

    if (ln && ln->valid && (ln->state == MESIState::E)) {
        // HIT in E -> transition to M and write
        resp.hit = true;
        ln->state = MESIState::M;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        touch_line(*ln);
        // need to inform others? if E then no sharers so no invalidation needed
        resp.busTrafficBytes = 0;
        return resp;
    }

    if (ln && ln->valid && ln->state == MESIState::S) {
        // HIT in S -> must issue BusUpgr to invalidate others, then go to M
        resp.hit = true;
        if (inter) inter->broadcast_busupgr(pe_id, block_number);
        // Invalidate others handled by interconnect; now upgrade
        ln->state = MESIState::M;
        size_t offset = address % WORDS_PER_BLOCK;
        ln->data[offset] = value;
        ln->dirty = true;
        touch_line(*ln);
        resp.busTrafficBytes = 0;
        resp.invalidations = 1; // approximate: we asked to invalidate
        return resp;
    }

    // MISS for store -> Write-allocate: bring block (BusRdX) and then write -> state M
    resp.hit = false;

    // Ask interconnect to invalidate others and/or supply (BusRdX)
    bool otherHad = false;
    if (inter) {
        inter->broadcast_busrdx(pe_id, block_number);
        // We don't get direct response here — snoops will have invalidated or written back if needed.
    }

    // Choose victim via LRU
    size_t victim_way = 0;
    uint64_t min_lru = UINT64_MAX;
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

    // Fill from memory (or from any flush done due to broadcast)
    victim.tag = tag;
    victim.valid = true;
    fill_line_from_memory(victim, block_number);

    // Now we have the block; since we did BusRdX we assume exclusive ownership (M)
    victim.state = MESIState::M;
    victim.dirty = true;
    size_t offset = address % WORDS_PER_BLOCK;
    victim.data[offset] = value;
    touch_line(victim);

    resp.busTrafficBytes += BLOCK_SIZE_BYTES;
    return resp;
}
