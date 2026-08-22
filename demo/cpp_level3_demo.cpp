/// Level 3 demo: serialize with nested vector data structure + arena
/// Same as bench: data is vector<vector<Slice>> (4 cols), arena bump allocator.
/// If this shows C++ slower than Rust → proves nested data structure is the trigger.
///
/// Build: clang++-22 -std=c++17 -O3 -march=armv8-a+crc -o demo/cpp_level3_demo demo/cpp_level3_demo.cpp
/// Run:   taskset -c 10 ./demo/cpp_level3_demo

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

// Same struct as bench TestData (nested vectors)
struct TestData {
    std::vector<std::vector<Slice>> slices; // 4 cols × 1M rows
    size_t totalRows;
};

// Arena — identical to OmniOperator
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
        chunks_.push_back(p); chunk_sizes_.push_back(size);
        buf_ = p; avail_ = size;
    }
public:
    Arena() : buf_(nullptr), avail_(0) {}
    ~Arena() { for (auto* p : chunks_) free(p); }
    uint8_t* allocate(size_t size) {
        if (avail_ < size) alloc_chunk(next_size(size));
        uint8_t* ret = buf_; buf_ += size; avail_ -= size; return ret;
    }
};

// SerializeVarcharToBuffer — OmniOperator original
inline size_t serialize_one(uint8_t* dst, const uint8_t* data, size_t len) {
    uint8_t row_len_size = (len <= 0xFF) ? 1 : (len <= 0xFFFF) ? 2 : 4;
    *dst = row_len_size;
    uint32_t l32 = static_cast<uint32_t>(len);
    memcpy(dst + 1, &l32, row_len_size);
    if (len) memcpy(dst + 1 + row_len_size, data, len);
    return 1 + row_len_size + len;
}

// Hot loop — same structure as BenchSerializeKey
__attribute__((noinline))
static uint64_t bench_serialize(const TestData& d) {
    Arena arena;
    volatile size_t numCols = NUM_COLS; // prevent unrolling
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < numCols; c++) {
            totalSize += 1 + ((d.slices[c][i].len <= 0xFF) ? 1 : (d.slices[c][i].len <= 0xFFFF) ? 2 : 4) + d.slices[c][i].len;
        }
        uint8_t* block = arena.allocate(totalSize);
        uint8_t* wp = block;
        for (size_t c = 0; c < numCols; c++) {
            wp += serialize_one(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

int main() {
    // Generate data — same as bench
    std::vector<std::vector<std::string>> str_data(NUM_COLS);
    TestData d;
    d.slices.resize(NUM_COLS);
    d.totalRows = NUM_ROWS;
    for (size_t c = 0; c < NUM_COLS; c++) {
        str_data[c].resize(NUM_ROWS);
        d.slices[c].resize(NUM_ROWS);
        for (size_t i = 0; i < NUM_ROWS; i++) {
            str_data[c][i] = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.slices[c][i].ptr = reinterpret_cast<const uint8_t*>(str_data[c][i].data());
            d.slices[c][i].len = str_data[c][i].size();
        }
    }

    volatile uint64_t w = bench_serialize(d); (void)w;
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t result = 0;
    for (size_t i = 0; i < NUM_ITERS; i++) result = bench_serialize(d);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / NUM_ITERS;
    printf("C++ level3 (nested vec + arena): %.2f ms  checksum=%lu\n", ms, (unsigned long)result);
    return 0;
}
