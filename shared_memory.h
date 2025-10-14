#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <cstdint>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <atomic>
#include <stdexcept>

// Tipo base
using Byte = uint8_t;

// Segmento de memoria asociado a un PE
struct Segment {
    int pe_id;
    uint32_t base_word;
    uint32_t len_words;
};

// Solicitud (usada entre Interconnect y SharedMemory)
struct Request {
    enum Type { READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK } type;
    uint32_t byte_addr;
    std::vector<Byte> data;
    std::shared_ptr<std::promise<uint64_t>> prom_word;
    std::shared_ptr<std::promise<std::vector<Byte>>> prom_block;
    std::shared_ptr<std::promise<void>> prom_void;
};

// Clase principal de memoria compartida
class SharedMemory {
public:
    explicit SharedMemory(uint32_t words);
    void add_segment(int pe_id, uint32_t base_word, uint32_t len_words);
    void start();
    void stop();

    // API asincrónica
    std::future<uint64_t> readWordAsync(uint32_t byte_addr);
    std::future<void> writeWordAsync(uint32_t byte_addr, uint64_t value);
    std::future<std::vector<Byte>> readBlockAsync(uint32_t byte_addr);
    std::future<void> writeBlockAsync(uint32_t byte_addr, const std::vector<Byte>& block32);

    // Estadísticas e inspección
    void dump_stats();
    int owner_segment(uint32_t byte_addr);

private:
    uint32_t size_words_;
    std::vector<uint64_t> mem_;
    std::vector<Segment> segments_;

    std::deque<Request> q_;
    std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::thread worker_;
    std::atomic<bool> running_;

    std::atomic<uint64_t> total_word_reads;
    std::atomic<uint64_t> total_word_writes;
    std::atomic<uint64_t> total_block_reads;
    std::atomic<uint64_t> total_block_writes;

    // Métodos internos
    void push_request(Request&& r);
    void worker_loop();
    void process_request(const Request& r);
    void req_set_value_word(std::shared_ptr<std::promise<uint64_t>> p, uint64_t val);
};

#endif // SHARED_MEMORY_H
