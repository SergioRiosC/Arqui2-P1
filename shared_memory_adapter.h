// shared_memory_adapter.h
#pragma once

#include "shared_memory.h"
#include "cache.hpp"   // para hw::kBlockBytes y la definición IMemory
#include <array>
#include <vector>
#include <cstring>
#include <cassert>

class SharedMemoryAdapter : public IMemory {
public:
    explicit SharedMemoryAdapter(SharedMemory* shm) : shm_(shm) {
        if (!shm_) throw std::runtime_error("SharedMemoryAdapter: shm == nullptr");
    }

    void writeBlockAligned(uint64_t block_addr, const std::array<uint8_t, hw::kBlockBytes>& data) override {
        // SharedMemory::writeBlockAsync espera byte_addr (32B aligned) y vector<Byte>
        std::vector<Byte> v(data.begin(), data.end());
        shm_->writeBlockAsync(static_cast<uint32_t>(block_addr), v).get();
    }

    void readBlockAligned(uint64_t block_addr, std::array<uint8_t, hw::kBlockBytes>& out) override {
        auto fut = shm_->readBlockAsync(static_cast<uint32_t>(block_addr));
        auto v = fut.get();
        if (v.size() != hw::kBlockBytes) throw std::runtime_error("SharedMemoryAdapter: block size mismatch");
        std::memcpy(out.data(), v.data(), hw::kBlockBytes);
    }

    double load64(uint64_t addr) override {
        auto fut = shm_->readWordAsync(static_cast<uint32_t>(addr));
        uint64_t raw = fut.get();
        double d;
        std::memcpy(&d, &raw, sizeof(d));
        return d;
    }

    void store64(uint64_t addr, double val) override {
        uint64_t raw;
        std::memcpy(&raw, &val, sizeof(raw));
        shm_->writeWordAsync(static_cast<uint32_t>(addr), raw).get();
    }

private:
    SharedMemory* shm_;
};
