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
constexpr size_t kBlockBytes = 32;
constexpr size_t kWays       = 2;
constexpr size_t kLines      = 16;
constexpr size_t kSets       = kLines / kWays;
static_assert(kSets == 8, "Deben ser 8 sets");

constexpr size_t kMemDoubles = 512;
constexpr size_t kMemBytes   = kMemDoubles * sizeof(uint64_t);
}

enum class MESI : uint8_t { Invalid=0, Shared=1, Exclusive=2, Modified=3 };

inline const char* mesi_str(MESI s);

enum class BusCmd : uint8_t { BusRd, BusRdX, BusUpgr, Flush };

struct BusMessage {
    BusCmd   cmd;
    uint64_t addr;
    int      src_pe;
};

class Cache;

struct SnoopSummary {
    bool shared_seen = false;
    bool mod_seen    = false;
};

struct IMemory {
    virtual ~IMemory() = default;
    virtual void writeBlockAligned(uint64_t block_addr, const std::array<uint8_t, hw::kBlockBytes>& data) = 0;
    virtual void readBlockAligned(uint64_t block_addr, std::array<uint8_t, hw::kBlockBytes>& out) = 0;
    virtual double load64(uint64_t addr) = 0;
    virtual void store64(uint64_t addr, double val) = 0;
};

struct AddrFields {
    uint64_t tag;
    uint32_t index;
    uint32_t offset;
};

struct Address {
    static constexpr uint32_t kOffBits = 5;
    static constexpr uint32_t kIdxBits = 3;
    static constexpr uint64_t kOffMask = (1u << kOffBits) - 1;
    static constexpr uint64_t kIdxMask = (1u << kIdxBits) - 1;

    static AddrFields split(uint64_t addr);
    static uint64_t block_base(uint64_t addr);
};

class Interconnect {
public:
    void register_cache(Cache* c);
    SnoopSummary broadcast(const BusMessage& msg, Cache* origin);
    void flush_all();

private:
    std::vector<Cache*> caches_;
    std::mutex m_;
    std::mutex bus_mutex_;
};

struct MESITransition {
    uint32_t set, way;
    MESI from, to;
    uint64_t tag;
    uint64_t addr;
};

struct SnoopResponse {
    bool had_copy    = false;
    bool wrote_back  = false;
};

struct CacheLine {
    MESI state = MESI::Invalid;
    uint64_t tag = 0;
    std::array<uint8_t, hw::kBlockBytes> data{};
    bool recent = false;
};

struct Stats {
    uint64_t read_ops  = 0;
    uint64_t write_ops = 0;
    uint64_t misses    = 0;
    uint64_t invalidations = 0;
    uint64_t bus_msgs  = 0;
};

class Cache {
public:
    Cache(int pe_id, IMemory* mem, Interconnect* ic);
    
    // API pública para el PE
    double read_double(uint64_t addr);
    void write_double(uint64_t addr, double value);
    
    // Snoop llamado por el interconnect
    SnoopResponse snoop(const BusMessage& msg);
    
    // Métricas y utilidades
    const Stats& stats() const { return stats_; }
    const std::vector<MESITransition>& transitions() const { return trans_; }
    int pe_id() const { return pe_id_; }
    void dump_state(std::ostream& os);
    void flush_all();
    MESI get_state(uint32_t set_idx, uint32_t way) const;
    uint64_t get_tag(uint32_t set_idx, uint32_t way) const;
    bool get_recent(uint32_t set_idx, uint32_t way) const;

private:
    friend class Interconnect;
    
    std::tuple<bool,uint32_t,uint32_t> probe(uint64_t tag, uint32_t set_idx) const;
    CacheLine& choose_victim(uint32_t set_idx);
    uint32_t victim_index(uint32_t set_idx) const;
    void mark_recent(uint32_t set_idx, uint32_t way);
    void evict_if_dirty(uint32_t set_idx, uint32_t& way, uint64_t new_addr);
    void fill_from_mem(uint64_t addr, uint32_t set_idx, uint32_t way);
    double load_from_line(uint32_t set_idx, uint32_t way, uint32_t off) const;
    void store_into_line(uint32_t set_idx, uint32_t way, uint32_t off, double v);
    void writeback_line(uint32_t set_idx, uint32_t way, uint64_t addr_for_block);
    uint64_t reconstruct_block_addr(uint64_t tag, uint32_t set_idx) const;
    void record_transition(uint32_t set, uint32_t way, MESI from, MESI to, uint64_t tag, uint64_t addr);

    int pe_id_;
    IMemory* mem_ = nullptr;
    Interconnect* ic_ = nullptr;
    std::vector<std::array<CacheLine, hw::kWays>> sets_;
    Stats stats_;
    std::vector<MESITransition> trans_;
    mutable std::mutex m_;
};