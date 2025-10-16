#include "shared_memory.h"
#include <iostream>
#include <cstring>

SharedMemory::SharedMemory(uint32_t words)
    : size_words_(words), mem_(words, 0),
      running_(false),
      total_word_reads(0), total_word_writes(0),
      total_block_reads(0), total_block_writes(0) {}

void SharedMemory::add_segment(int pe_id, uint32_t base_word, uint32_t len_words) {
    Segment s{pe_id, base_word, len_words};
    segments_.push_back(s);
}

void SharedMemory::start() {
    running_ = true;
    worker_ = std::thread(&SharedMemory::worker_loop, this);
}

void SharedMemory::stop() {
    {
        std::unique_lock<std::mutex> lk(q_mutex_);
        running_ = false;
        q_cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

std::future<uint64_t> SharedMemory::readWordAsync(uint32_t byte_addr) {
    auto p = std::make_shared<std::promise<uint64_t>>();
    Request r;
    r.type = Request::READ_WORD;
    r.byte_addr = byte_addr;
    r.prom_word = p;
    push_request(std::move(r));
    return p->get_future();
}

std::future<void> SharedMemory::writeWordAsync(uint32_t byte_addr, uint64_t value) {
    auto p = std::make_shared<std::promise<void>>();
    Request r;
    r.type = Request::WRITE_WORD;
    r.byte_addr = byte_addr;
    r.data.resize(8);
    memcpy(r.data.data(), &value, 8);
    r.prom_void = p;
    push_request(std::move(r));
    return p->get_future();
}

std::future<std::vector<Byte>> SharedMemory::readBlockAsync(uint32_t byte_addr) {
    auto p = std::make_shared<std::promise<std::vector<Byte>>>();
    Request r;
    r.type = Request::READ_BLOCK;
    r.byte_addr = byte_addr;
    r.prom_block = p;
    push_request(std::move(r));
    return p->get_future();
}

std::future<void> SharedMemory::writeBlockAsync(uint32_t byte_addr, const std::vector<Byte>& block32) {
    auto p = std::make_shared<std::promise<void>>();
    Request r;
    r.type = Request::WRITE_BLOCK;
    r.byte_addr = byte_addr;
    r.data = block32;
    r.prom_void = p;
    push_request(std::move(r));
    return p->get_future();
}

void SharedMemory::dump_stats() {
    std::cout << "SHM stats: word_reads=" << total_word_reads.load()
              << " word_writes=" << total_word_writes.load()
              << " block_reads=" << total_block_reads.load()
              << " block_writes=" << total_block_writes.load() << "\n";
}

int SharedMemory::owner_segment(uint32_t byte_addr) {
    uint32_t word = byte_addr / 8;
    for (auto &s : segments_) {
        if (word >= s.base_word && word < s.base_word + s.len_words) return s.pe_id;
    }
    return -1;
}

void SharedMemory::push_request(Request&& r) {
    std::unique_lock<std::mutex> lk(q_mutex_);
    q_.push_back(std::move(r));
    q_cv_.notify_one();
}

void SharedMemory::worker_loop() {
    while (true) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(q_mutex_);
            q_cv_.wait(lk, [&]{ return !q_.empty() || !running_; });
            if (!running_ && q_.empty()) break;
            req = std::move(q_.front());
            q_.pop_front();
        }
        try {
            process_request(req);
        } catch (...) {
            if (req.prom_word) req.prom_word->set_exception(std::current_exception());
            if (req.prom_block) req.prom_block->set_exception(std::current_exception());
            if (req.prom_void) req.prom_void->set_exception(std::current_exception());
        }
    }
}

void SharedMemory::process_request(const Request& r) {
    if (r.type == Request::READ_WORD || r.type == Request::WRITE_WORD) {
        if (r.byte_addr % 8 != 0) throw std::runtime_error("Unaligned word access");
        uint32_t word_idx = r.byte_addr / 8;
        if (word_idx >= size_words_) throw std::runtime_error("Word address out of range");

        if (r.type == Request::READ_WORD) {
            uint64_t val = mem_[word_idx];
            total_word_reads.fetch_add(1);
            req_set_value_word(r.prom_word, val);
        } else {
            if (r.data.size() != 8) throw std::runtime_error("WRITE_WORD needs 8 bytes");
            uint64_t val;
            memcpy(&val, r.data.data(), 8);
            mem_[word_idx] = val;
            total_word_writes.fetch_add(1);
            if (r.prom_void) r.prom_void->set_value();
        }
    } else {
        if (r.byte_addr % 32 != 0) throw std::runtime_error("Unaligned block access");
        uint32_t block_idx = (r.byte_addr / 8) / 4;
        uint32_t first_word = block_idx * 4;
        if (first_word + 4 > size_words_) throw std::runtime_error("Block address out of range");

        if (r.type == Request::READ_BLOCK) {
            std::vector<Byte> out(32);
            for (int i = 0; i < 4; ++i) {
                uint64_t w = mem_[first_word + i];
                memcpy(out.data() + i*8, &w, 8);
            }
            total_block_reads.fetch_add(1);
            if (r.prom_block) r.prom_block->set_value(out);
        } else {
            if (r.data.size() != 32) throw std::runtime_error("WRITE_BLOCK needs 32 bytes");
            for (int i = 0; i < 4; ++i) {
                uint64_t w;
                memcpy(&w, r.data.data() + i*8, 8);
                mem_[first_word + i] = w;
            }
            total_block_writes.fetch_add(1);
            if (r.prom_void) r.prom_void->set_value();
        }
    }
}

void SharedMemory::req_set_value_word(std::shared_ptr<std::promise<uint64_t>> p, uint64_t val) {
    if (p) p->set_value(val);
}