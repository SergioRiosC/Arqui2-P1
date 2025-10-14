#include "shared_memory.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cstring>

static inline uint64_t double_to_u64(double d) { uint64_t u; std::memcpy(&u, &d, sizeof(u)); return u; }
static inline double u64_to_double(uint64_t u) { double d; std::memcpy(&d, &u, sizeof(d)); return d; }

int main() {
    const uint32_t MEM_WORDS = 512;
    const uint32_t N = 56;

    if (2 * N + 9 > MEM_WORDS) {
        std::cerr << "N demasiado grande para la memoria disponible\n";
        return 1;
    }

    const uint32_t A_base_word = 0;
    const uint32_t B_base_word = A_base_word + N;
    const uint32_t PARTIAL_base_word = B_base_word + N;
    const uint32_t FLAGS_base_word = PARTIAL_base_word + 4;
    const uint32_t FINAL_word = FLAGS_base_word + 4;

    SharedMemory shm(MEM_WORDS);

    shm.add_segment(0, 0, 128);
    shm.add_segment(1, 128, 128);
    shm.add_segment(2, 256, 128);
    shm.add_segment(3, 384, 128);

    shm.start();

    // Inicializar vectores A y B
    for (uint32_t i = 0; i < N; ++i) {
        double a = static_cast<double>(i) + 1.0;
        double b = 2.0;
        shm.writeWordAsync((A_base_word + i) * 8, double_to_u64(a)).get();
        shm.writeWordAsync((B_base_word + i) * 8, double_to_u64(b)).get();
    }

    // 4 PEs
    std::vector<std::thread> pes;
    for (int pe = 0; pe < 4; ++pe) {
        pes.emplace_back([pe, &shm, N, A_base_word, B_base_word, PARTIAL_base_word, FLAGS_base_word]() {
            uint32_t chunk = N / 4;
            uint32_t start = pe * chunk;
            uint32_t end = (pe == 3) ? N : start + chunk;
            double sum = 0.0;

            for (uint32_t idx = start; idx < end; ++idx) {
                uint64_t au = shm.readWordAsync((A_base_word + idx) * 8).get();
                uint64_t bu = shm.readWordAsync((B_base_word + idx) * 8).get();
                sum += u64_to_double(au) * u64_to_double(bu);
            }

            shm.writeWordAsync((PARTIAL_base_word + pe) * 8, double_to_u64(sum)).get();
            shm.writeWordAsync((FLAGS_base_word + pe) * 8, 1ULL).get();
        });
    }

    // Agregador
    double final_res = 0.0;
    bool ready = false;

    while (!ready) {
        ready = true;
        for (int i = 0; i < 4; ++i) {
            uint64_t f = shm.readWordAsync((FLAGS_base_word + i) * 8).get();
            if (f == 0) { ready = false; break; }
        }
        if (!ready)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (int i = 0; i < 4; ++i) {
        uint64_t pu = shm.readWordAsync((PARTIAL_base_word + i) * 8).get();
        final_res += u64_to_double(pu);
    }

    shm.writeWordAsync(FINAL_word * 8, double_to_u64(final_res)).get();

    for (auto &t : pes)
        if (t.joinable()) t.join();

    double seq = 0.0;
    for (uint32_t i = 0; i < N; ++i) {
        uint64_t au = shm.readWordAsync((A_base_word + i) * 8).get();
        uint64_t bu = shm.readWordAsync((B_base_word + i) * 8).get();
        seq += u64_to_double(au) * u64_to_double(bu);
    }

    std::cout.setf(std::ios::fixed);
    std::cout << std::setprecision(6);
    std::cout << "Producto punto (simulado): " << final_res << "\n";
    std::cout << "Producto punto (secuencial): " << seq << "\n";

    shm.dump_stats();
    shm.stop();
    return 0;
}
