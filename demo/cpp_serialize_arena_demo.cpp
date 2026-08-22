/// Demo: serialize 4 short strings WITH bump-pointer arena allocation per row.
/// This is the exact pattern that causes backend stall on Kunpeng.
///
/// Build: clang++-22 -std=c++17 -O3 -march=armv8-a+crc -o demo/cpp_serialize_arena_demo demo/cpp_serialize_arena_demo.cpp
/// Run:   taskset -c 10 ./demo/cpp_serialize_arena_demo

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>

static constexpr size_t NUM_ROWS = 1000000;
static constexpr size_t NUM_COLS = 4;
static constexpr size_t NUM_ITERS = 10;

struct Slice { const uint8_t* ptr; size_t len; };

// Simple bump-pointer arena — same as OmniOperator's SimpleArenaAllocator
class Arena {
    uint8_t* buf_;
    size_t avail_;
    size_t min_chunk_ = 4096;
    size_t growth_factor_ = 2;
    size_t linear_threshold_ = 512 * 1024;
    std::vector<uint8_t*> chunks_;
    std::vector<size_t> chunk_sizes_;

    size_t next_size(size_t needed) {
        if (chunks_.empty()) return std::max(needed, min_chunk_);
        size_t last = chunk_sizes_.back();
        if (last < linear_threshold_) return std::max(needed, last * growth_factor_);
        return ((needed + linear_threshold_ - 1) / linear_threshold_) * linear_threshold_;
    }
    void alloc_chunk(size_t size) {
        auto* p = static_cast<uint8_t*>(malloc(size));
        chunks_.push_back(p);
        chunk_sizes_.push_back(size);
        buf_ = p;
        avail_ = size;
    }
public:
    Arena() : buf_(nullptr), avail_(0) {}
    ~Arena() { for (auto* p : chunks_) free(p); }

    uint8_t* allocate(size_t size) {
        if (avail_ < size) alloc_chunk(next_size(size));
        uint8_t* ret = buf_;
        buf_ += size;
        avail_ -= size;
        return ret;
    }
};

// SerializeVarcharToBuffer — identical to OmniOperator
inline size_t serialize_one(uint8_t* dst, const uint8_t* data, size_t len) {
    uint8_t row_len_size = (len <= 0xFF) ? 1 : (len <= 0xFFFF) ? 2 : 4;
    *dst = row_len_size;
    uint32_t l32 = static_cast<uint32_t>(len);
    memcpy(dst + 1, &l32, row_len_size);
    if (len) memcpy(dst + 1 + row_len_size, data, len);
    return 1 + row_len_size + len;
}

// Hot loop: arena allocate + serialize 4 columns per row
__attribute__((noinline))
uint64_t bench_serialize_arena(const std::vector<std::vector<Slice>>& cols) {
    Arena arena;
    uint64_t checksum = 0;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        // Compute total size for 4 columns
        size_t total = 0;
        for (size_t c = 0; c < NUM_COLS; c++) {
            size_t len = cols[c][i].len;
            uint8_t rls = (len <= 0xFF) ? 1 : (len <= 0xFFFF) ? 2 : 4;
            total += 1 + rls + len;
        }
        // Arena allocate
        uint8_t* block = arena.allocate(total);
        uint8_t* wp = block;
        // Serialize 4 columns
        for (size_t c = 0; c < NUM_COLS; c++) {
            wp += serialize_one(wp, cols[c][i].ptr, cols[c][i].len);
        }
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

int main() {
    // Generate data
    std::vector<std::vector<std::string>> str_data(NUM_COLS);
    std::vector<std::vector<Slice>> cols(NUM_COLS);
    for (size_t c = 0; c < NUM_COLS; c++) {
        str_data[c].resize(NUM_ROWS);
        cols[c].resize(NUM_ROWS);
        for (size_t i = 0; i < NUM_ROWS; i++) {
            str_data[c][i] = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            cols[c][i].ptr = reinterpret_cast<const uint8_t*>(str_data[c][i].data());
            cols[c][i].len = str_data[c][i].size();
        }
    }

    // Warmup
    volatile uint64_t w = bench_serialize_arena(cols); (void)w;

    // Measure
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t result = 0;
    for (size_t i = 0; i < NUM_ITERS; i++) {
        result = bench_serialize_arena(cols);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / NUM_ITERS;

    printf("C++ serialize+arena: %.2f ms  checksum=%lu\n", ms, (unsigned long)result);
    return 0;
}
