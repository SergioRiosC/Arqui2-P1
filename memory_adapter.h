#ifndef MEMORY_ADAPTER_H
#define MEMORY_ADAPTER_H

#include "shared_memory.h"
#include <array>
#include <cstring>

class SharedMemoryAdapter {
public:
    explicit SharedMemoryAdapter(SharedMemory* shm) : shm_(shm) {}

    void readBlockAligned(uint64_t block_addr, std::array<uint8_t, 32>& out) {
        auto vec = shm_->readBlockAsync(block_addr).get();
        std::memcpy(out.data(), vec.data(), 32);
    }

    void writeBlockAligned(uint64_t block_addr, const std::array<uint8_t, 32>& data) {
        std::vector<uint8_t> v(32);
        std::memcpy(v.data(), data.data(), 32);
        shm_->writeBlockAsync(block_addr, v).get();
    }

    double load64(uint64_t addr) {
        uint64_t val = shm_->readWordAsync(addr).get();
        double d;
        std::memcpy(&d, &val, 8);
        return d;
    }

    void store64(uint64_t addr, double val) {
        uint64_t u;
        std::memcpy(&u, &val, 8);
        shm_->writeWordAsync(addr, u).get();
    }

private:
    SharedMemory* shm_;
};

#endif
