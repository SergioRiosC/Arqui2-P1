#pragma once

#include "shared_memory.h"
#include "cache.hpp"   // para hw::kBlockBytes y la definición IMemory
#include <array>
#include <vector>
#include <cstring>
#include <cassert>

// ADAPTADOR DE MEMORIA - Conecta el sistema de caché con la memoria compartida
// Implementa la interfaz IMemory que esperan las caches
class SharedMemoryAdapter : public IMemory {
public:
    explicit SharedMemoryAdapter(SharedMemory* shm) : shm_(shm) {
        if (!shm_) throw std::runtime_error("SharedMemoryAdapter: shm == nullptr");
    }

    // ESCRITURA DE BLOQUE - 32 bytes alineados
    void writeBlockAligned(uint64_t block_addr, 
                          const std::array<uint8_t, hw::kBlockBytes>& data) override {
        // Convertir array a vector y escribir asíncronamente
        std::vector<Byte> v(data.begin(), data.end());
        shm_->writeBlockAsync(static_cast<uint32_t>(block_addr), v).get(); // Esperar completar
    }

    // LECTURA DE BLOQUE - 32 bytes alineados  
    void readBlockAligned(uint64_t block_addr, 
                         std::array<uint8_t, hw::kBlockBytes>& out) override {
        auto fut = shm_->readBlockAsync(static_cast<uint32_t>(block_addr));
        auto v = fut.get(); // Esperar y obtener resultado
        if (v.size() != hw::kBlockBytes) 
            throw std::runtime_error("SharedMemoryAdapter: block size mismatch");
        std::memcpy(out.data(), v.data(), hw::kBlockBytes);
    }

    // LECTURA DE DOUBLE - 8 bytes
    double load64(uint64_t addr) override {
        auto fut = shm_->readWordAsync(static_cast<uint32_t>(addr));
        uint64_t raw = fut.get(); // Obtener valor crudo
        double d;
        std::memcpy(&d, &raw, sizeof(d)); // Convertir bits a double
        return d;
    }

    // ESCRITURA DE DOUBLE - 8 bytes
    void store64(uint64_t addr, double val) override {
        uint64_t raw;
        std::memcpy(&raw, &val, sizeof(raw)); // Convertir double a bits
        shm_->writeWordAsync(static_cast<uint32_t>(addr), raw).get(); // Escribir
    }

private:
    SharedMemory* shm_; // Puntero a la memoria compartida real
};