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

using Byte = uint8_t;

// SEGMENTO DE MEMORIA - Para particionamiento lógico
struct Segment {
    int pe_id;          // PE dueño del segmento
    uint32_t base_word; // Dirección base en palabras
    uint32_t len_words; // Longitud en palabras
};

// SOLICITUD DE MEMORIA - Para comunicación asíncrona
struct Request {
    enum Type { READ_WORD, WRITE_WORD, READ_BLOCK, WRITE_BLOCK } type;
    uint32_t byte_addr;     // Dirección en bytes
    std::vector<Byte> data; // Datos a escribir/leídos
    std::shared_ptr<std::promise<uint64_t>> prom_word;     // Promesa para lectura palabra
    std::shared_ptr<std::promise<std::vector<Byte>>> prom_block; // Promesa para lectura bloque
    std::shared_ptr<std::promise<void>> prom_void;         // Promesa para escritura
};

// MEMORIA COMPARTIDA - Memoria principal con acceso asíncrono
class SharedMemory {
public:
    explicit SharedMemory(uint32_t words); // Constructor con tamaño en palabras

    // Gestión de segmentos
    void add_segment(int pe_id, uint32_t base_word, uint32_t len_words);
    
    // Control del sistema
    void start(); // Iniciar hilo worker
    void stop();  // Detener hilo worker

    // API ASÍNCRONA para acceso a memoria
    std::future<uint64_t> readWordAsync(uint32_t byte_addr);
    std::future<void> writeWordAsync(uint32_t byte_addr, uint64_t value);
    std::future<std::vector<Byte>> readBlockAsync(uint32_t byte_addr);
    std::future<void> writeBlockAsync(uint32_t byte_addr, const std::vector<Byte>& block32);

    // Utilidades
    void dump_stats(); // Mostrar estadísticas
    int owner_segment(uint32_t byte_addr); // Encontrar dueño de dirección

private:
    uint32_t size_words_;           // Tamaño total en palabras
    std::vector<uint64_t> mem_;     // Almacenamiento principal
    std::vector<Segment> segments_; // Segmentos definidos

    // COLA DE SOLICITUDES para procesamiento asíncrono
    std::deque<Request> q_;
    std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::thread worker_;
    std::atomic<bool> running_;

    // ESTADÍSTICAS de uso
    std::atomic<uint64_t> total_word_reads;
    std::atomic<uint64_t> total_word_writes;
    std::atomic<uint64_t> total_block_reads;
    std::atomic<uint64_t> total_block_writes;

    // MÉTODOS INTERNOS
    void push_request(Request&& r);     // Agregar solicitud a la cola
    void worker_loop();                 // Loop del hilo worker
    void process_request(const Request& r); // Procesar una solicitud
    void req_set_value_word(std::shared_ptr<std::promise<uint64_t>> p, uint64_t val);
};

#endif