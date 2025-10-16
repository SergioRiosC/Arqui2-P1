#include "shared_memory.h"
#include "memory_adapter.h"
#include <iostream>
#include <iomanip>

int main() {
    SharedMemory shm(512);
    SharedMemoryAdapter mem(&shm);
    shm.start();

    const uint64_t addr = 0;
    double value = 42.123;
    mem.store64(addr, value);
    double read_back = mem.load64(addr);

    std::cout << std::fixed << std::setprecision(6);
    std::cout << "Valor escrito: " << value << "\n";
    std::cout << "Valor leído  : " << read_back << "\n";

    shm.dump_stats();
    shm.stop();
}
