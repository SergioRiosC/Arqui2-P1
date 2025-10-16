#include "shared_memory.h"
#include "memory_adapter.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstring>

static inline uint64_t double_to_u64(double d) { uint64_t u; memcpy(&u, &d, sizeof(u)); return u; }
static inline double u64_to_double(uint64_t u) { double d; memcpy(&d, &u, sizeof(d)); return d; }

int main() {
    const uint32_t MEM_WORDS = 512;
    const uint32_t N = 200; // tamaño del vector

    // Layout en memoria (en palabras de 8 bytes)
    const uint32_t A_base = 0;
    const uint32_t B_base = A_base + N;
    const uint32_t PARTIAL_base = B_base + N;
    const uint32_t FLAGS_base = PARTIAL_base + 4;
    const uint32_t FINAL_word = FLAGS_base + 4;

    SharedMemory shm(MEM_WORDS);
    SharedMemoryAdapter mem(&shm);

    shm.add_segment(0, 0, 128);
    shm.add_segment(1, 128, 128);
    shm.add_segment(2, 256, 128);
    shm.add_segment(3, 384, 128);
    shm.start();

    // Inicializar vectores A y B
    for (uint32_t i = 0; i < N; ++i) {
        double a = static_cast<double>(i) + 1.0;
        double b = 2.0;
        shm.writeWordAsync((A_base + i) * 8, double_to_u64(a)).get();
        shm.writeWordAsync((B_base + i) * 8, double_to_u64(b)).get();
    }

    // Inicializar banderas en 0
    for (int i = 0; i < 4; ++i)
        shm.writeWordAsync((FLAGS_base + i) * 8, 0ULL).get();

    // Hilos simulando 4 PEs
    std::vector<std::thread> pes;
    for (int pe = 0; pe < 4; ++pe) {
        pes.emplace_back([pe, &shm, N, A_base, B_base, PARTIAL_base, FLAGS_base]() {
            uint32_t chunk = N / 4;
            uint32_t start = pe * chunk;
            uint32_t end = (pe == 3) ? N : start + chunk;
            double sum = 0.0;

            // Leer pares A[i], B[i]
            for (uint32_t i = start; i < end; ++i) {
                uint64_t au = shm.readWordAsync((A_base + i) * 8).get();
                uint64_t bu = shm.readWordAsync((B_base + i) * 8).get();
                sum += u64_to_double(au) * u64_to_double(bu);
            }

            // Guardar resultado parcial y marcar bandera
            shm.writeWordAsync((PARTIAL_base + pe) * 8, double_to_u64(sum)).get();
            shm.writeWordAsync((FLAGS_base + pe) * 8, 1ULL).get();
        });
    }

    // PE agregador
    double final_res = 0.0;
    bool ready = false;

    while (!ready) {
        ready = true;
        for (int i = 0; i < 4; ++i) {
            uint64_t f = shm.readWordAsync((FLAGS_base + i) * 8).get();
            if (f == 0) { ready = false; break; }
        }
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 4; ++i) {
        uint64_t pu = shm.readWordAsync((PARTIAL_base + i) * 8).get();
        final_res += u64_to_double(pu);
    }

    shm.writeWordAsync(FINAL_word * 8, double_to_u64(final_res)).get();

    for (auto &t : pes)
        if (t.joinable()) t.join();

    // Calcular referencia secuencial
    double seq = 0.0;
    for (uint32_t i = 0; i < N; ++i) {
        uint64_t au = shm.readWordAsync((A_base + i) * 8).get();
        uint64_t bu = shm.readWordAsync((B_base + i) * 8).get();
        seq += u64_to_double(au) * u64_to_double(bu);
    }

    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(6);
    std::cout << "Producto punto (paralelo simulado): " << final_res << "\n";
    std::cout << "Producto punto (secuencial): " << seq << "\n";

    shm.dump_stats();
    shm.stop();
    return 0;
}