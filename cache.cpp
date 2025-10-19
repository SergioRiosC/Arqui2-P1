// cache.cpp
#include "cache.hpp"

// Definir el mutex global
std::mutex io_mtx;

// Implementaciones de funciones inline
const char* mesi_str(MESI s) {
    switch (s) {
        case MESI::Invalid:   return "I";
        case MESI::Shared:    return "S";
        case MESI::Exclusive: return "E";
        case MESI::Modified:  return "M";
    }
    return "?";
}

AddrFields Address::split(uint64_t addr) {
    uint64_t off = addr & Address::kOffMask;
    uint64_t idx = (addr >> Address::kOffBits) & Address::kIdxMask;
    uint64_t tag = (addr >> (Address::kOffBits + Address::kIdxBits));
    return {tag, static_cast<uint32_t>(idx), static_cast<uint32_t>(off)};
}

uint64_t Address::block_base(uint64_t addr) {
    return addr & ~static_cast<uint64_t>(Address::kOffMask);
}

// Implementaciones de Interconnect
void Interconnect::register_cache(Cache* c) {
    std::lock_guard<std::mutex> lk(m_);
    caches_.push_back(c);
}

SnoopSummary Interconnect::broadcast(const BusMessage& msg, Cache* origin) {
    std::unique_lock<std::mutex> buslk(bus_mutex_);
    std::vector<Cache*> local;
    {
        std::lock_guard<std::mutex> lk(m_);
        local = caches_;
    }
    SnoopSummary sum{};
    for (auto* c : local) {
        if (c == origin) continue;
        auto resp = c->snoop(msg);
        sum.shared_seen = sum.shared_seen || resp.had_copy;
        sum.mod_seen    = sum.mod_seen    || resp.wrote_back;
    }
    return sum;
}

void Interconnect::flush_all() {
    std::unique_lock<std::mutex> buslk(bus_mutex_);
    std::vector<Cache*> local;
    {
        std::lock_guard<std::mutex> lk(m_);
        local = caches_;
    }
    for (auto* c : local) c->flush_all();
}

// Implementaciones de Cache
Cache::Cache(int pe_id, IMemory* mem, Interconnect* ic)
    : pe_id_(pe_id), mem_(mem), ic_(ic) {
    sets_.resize(hw::kSets);
    for (auto& set : sets_) set.fill(CacheLine{});
    if (ic_) ic_->register_cache(this);
}

double Cache::read_double(uint64_t addr) {
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
            mark_recent(set_idx, way);
            return load_from_line(set_idx, way, f.offset);
        }
    }

    BusMessage m{BusCmd::BusRd, addr, pe_id_};
    stats_.bus_msgs++;
    SnoopSummary sum = ic_ ? ic_->broadcast(m, this) : SnoopSummary{};

    std::lock_guard<std::mutex> lk(m_);
    auto f2 = Address::split(addr);
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

void Cache::write_double(uint64_t addr, double value) {
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
            if (cur_state == MESI::Exclusive) {
                record_transition(set_idx, way, MESI::Exclusive, MESI::Modified, f.tag, addr);
                sets_[set_idx][way].state = MESI::Modified;
            }
            if (cur_state != MESI::Shared) {
                store_into_line(set_idx, way, f.offset, value);
                mark_recent(set_idx, way);
                return;
            }
        }
    }

    auto f2 = Address::split(addr);
    if (cur_state == MESI::Shared) {
        BusMessage m{BusCmd::BusUpgr, addr, pe_id_};
        stats_.bus_msgs++;
        if (ic_) ic_->broadcast(m, this);
        
        std::lock_guard<std::mutex> lk(m_);
        auto [hit2, sidx2, w2] = probe(f2.tag, f2.index);
        uint32_t use_way = hit2 ? w2 : victim_index(sidx2);
        record_transition(sidx2, use_way, MESI::Shared, MESI::Modified, f2.tag, addr);
        sets_[sidx2][use_way].state = MESI::Modified;
        sets_[sidx2][use_way].tag = f2.tag;
        store_into_line(sidx2, use_way, f2.offset, value);
        mark_recent(sidx2, use_way);
        return;
    } else {
        BusMessage m{BusCmd::BusRdX, addr, pe_id_};
        stats_.bus_msgs++;
        if (ic_) ic_->broadcast(m, this);

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

SnoopResponse Cache::snoop(const BusMessage& msg) {
    std::lock_guard<std::mutex> lk(m_);
    auto f = Address::split(msg.addr);
    auto [hit, set_idx, way] = probe(f.tag, f.index);
    SnoopResponse resp;

    if (!hit) return resp;

    auto& line = sets_[set_idx][way];
    resp.had_copy = (line.state != MESI::Invalid);

    switch (msg.cmd) {
        case BusCmd::BusRd:
            if (line.state == MESI::Modified) {
                writeback_line(set_idx, way, msg.addr);
                resp.wrote_back = true;
                record_transition(set_idx, way, MESI::Modified, MESI::Shared, f.tag, msg.addr);
                line.state = MESI::Shared;
            } else if (line.state == MESI::Exclusive) {
                record_transition(set_idx, way, MESI::Exclusive, MESI::Shared, f.tag, msg.addr);
                line.state = MESI::Shared;
            }
            break;

        case BusCmd::BusRdX:
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
            if (line.state == MESI::Shared || line.state == MESI::Exclusive) {
                stats_.invalidations++;
                record_transition(set_idx, way, line.state, MESI::Invalid, f.tag, msg.addr);
                line.state = MESI::Invalid;
            }
            break;

        case BusCmd::Flush:
            break;
    }
    return resp;
}

void Cache::dump_state(std::ostream& os) {
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

void Cache::flush_all() {
    std::lock_guard<std::mutex> lk(m_);
    for (uint32_t s = 0; s < hw::kSets; ++s) {
        for (uint32_t w = 0; w < hw::kWays; ++w) {
            auto &line = sets_[s][w];
            if (line.state == MESI::Modified) {
                uint64_t block_addr = reconstruct_block_addr(line.tag, s);
                mem_->writeBlockAligned(block_addr, line.data);
                line.state = MESI::Exclusive;
            }
        }
    }
}

MESI Cache::get_state(uint32_t set_idx, uint32_t way) const {
    std::lock_guard<std::mutex> lk(m_);
    return sets_[set_idx][way].state;
}

uint64_t Cache::get_tag(uint32_t set_idx, uint32_t way) const {
    std::lock_guard<std::mutex> lk(m_);
    return sets_[set_idx][way].tag;
}

bool Cache::get_recent(uint32_t set_idx, uint32_t way) const {
    std::lock_guard<std::mutex> lk(m_);
    return sets_[set_idx][way].recent;
}

// Métodos privados
std::tuple<bool,uint32_t,uint32_t> Cache::probe(uint64_t tag, uint32_t set_idx) const {
    for (uint32_t w=0; w<hw::kWays; ++w) {
        const auto& line = sets_[set_idx][w];
        if (line.state != MESI::Invalid && line.tag == tag) {
            return {true,set_idx,w};
        }
    }
    return {false,set_idx,0};
}

CacheLine& Cache::choose_victim(uint32_t set_idx) {
    uint32_t w0 = 0, w1 = 1;
    if      (!sets_[set_idx][w0].recent &&  sets_[set_idx][w1].recent) return sets_[set_idx][w0];
    else if ( sets_[set_idx][w0].recent && !sets_[set_idx][w1].recent) return sets_[set_idx][w1];
    else return sets_[set_idx][0];
}

uint32_t Cache::victim_index(uint32_t set_idx) const {
    uint32_t w0 = 0, w1 = 1;
    if      (!sets_[set_idx][w0].recent &&  sets_[set_idx][w1].recent) return w0;
    else if ( sets_[set_idx][w0].recent && !sets_[set_idx][w1].recent) return w1;
    else return 0;
}

void Cache::mark_recent(uint32_t set_idx, uint32_t way) {
    sets_[set_idx][way].recent = true;
    sets_[set_idx][1 - way].recent = false;
}

void Cache::evict_if_dirty(uint32_t set_idx, uint32_t& way, uint64_t new_addr) {
    way = victim_index(set_idx);
    auto& line = sets_[set_idx][way];
    if (line.state == MESI::Modified) {
        uint64_t old_block_addr = reconstruct_block_addr(line.tag, set_idx);
        mem_->writeBlockAligned(old_block_addr, line.data);
    }
    line.state = MESI::Invalid;
    line.tag   = 0;
    line.recent = false;
}

void Cache::fill_from_mem(uint64_t addr, uint32_t set_idx, uint32_t way) {
    auto block_addr = Address::block_base(addr);
    mem_->readBlockAligned(block_addr, sets_[set_idx][way].data);
}

double Cache::load_from_line(uint32_t set_idx, uint32_t way, uint32_t off) const {
    double val;
    std::memcpy(&val, &sets_[set_idx][way].data[off], 8);
    return val;
}

void Cache::store_into_line(uint32_t set_idx, uint32_t way, uint32_t off, double v) {
    std::memcpy(&sets_[set_idx][way].data[off], &v, 8);
}

void Cache::writeback_line(uint32_t set_idx, uint32_t way, uint64_t addr_for_block) {
    uint64_t block_addr = Address::block_base(addr_for_block);
    mem_->writeBlockAligned(block_addr, sets_[set_idx][way].data);
}

uint64_t Cache::reconstruct_block_addr(uint64_t tag, uint32_t set_idx) const {
    uint64_t addr = (tag << (Address::kOffBits + Address::kIdxBits)) |
                    (static_cast<uint64_t>(set_idx) << Address::kOffBits);
    return addr;
}

void Cache::record_transition(uint32_t set, uint32_t way, MESI from, MESI to, uint64_t tag, uint64_t addr) {
    if (from == to) return;
    trans_.push_back(MESITransition{set,way,from,to,tag,addr});
}

// Demo opcional
#ifdef CACHE_DEMO
#include "shared_memory.h"
#include "shared_memory_adapter.h"

int main() {
    SharedMemory mem(512);
    mem.start();
    SharedMemoryAdapter mem_adapter(&mem);
    Interconnect bus;
    Cache c0(0, &mem_adapter, &bus);
    Cache c1(1, &mem_adapter, &bus);

    mem_adapter.store64(0,  1.5);
    mem_adapter.store64(8,  2.0);

    double a = c0.read_double(0);
    c1.write_double(8, 3.14159);
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

    for (auto& t : c1.transitions()) {
        std::cout << "PE#" << c1.pe_id() << " set=" << t.set
                  << " way=" << t.way
                  << " " << mesi_str(t.from) << "->" << mesi_str(t.to)
                  << " tag=0x" << std::hex << t.tag << std::dec
                  << " addr=0x" << std::hex << t.addr << std::dec << "\n";
    }
    
    mem.stop();
    return 0;
}
#endif