/// Minimal demo: serialize 4 short strings into a buffer, 1M iterations.
/// Proves that clang inlines the inner function → large function → backend stall on Kunpeng.
///
/// Build: clang++ -O3 -march=armv8-a+crc -o cpp_serialize_demo cpp_serialize_demo.cpp
/// Run:   taskset -c 10 ./cpp_serialize_demo

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>
#include <string>

static constexpr size_t NUM_ROWS = 1000000;
static constexpr size_t NUM_COLS = 4;
static constexpr size_t NUM_ITERS = 10;

struct Slice { const uint8_t* ptr; size_t len; };

// Mirrors OmniOperator's SerializeVarcharToBuffer — inline, uses memcpy
inline size_t serialize_one(uint8_t* dst, const uint8_t* data, size_t len) {
    uint8_t row_len_size = (len <= 0xFF) ? 1 : (len <= 0xFFFF) ? 2 : 4;
    *dst = row_len_size;
    uint32_t l32 = static_cast<uint32_t>(len);
    memcpy(dst + 1, &l32, row_len_size);
    if (len) memcpy(dst + 1 + row_len_size, data, len);
    return 1 + row_len_size + len;
}

// Hot loop: serialize 4 columns per row, N rows
// The inner serialize_one is inline — compiler decides whether to inline it.
__attribute__((noinline))
uint64_t bench_serialize(const std::vector<std::vector<Slice>>& cols, uint8_t* arena, size_t arena_size) {
    uint64_t checksum = 0;
    size_t offset = 0;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        // Compute total size for this row
        size_t total = 0;
        for (size_t c = 0; c < NUM_COLS; c++) {
            size_t len = cols[c][i].len;
            uint8_t rls = (len <= 0xFF) ? 1 : (len <= 0xFFFF) ? 2 : 4;
            total += 1 + rls + len;
        }
        // Reset if near end
        if (offset + total > arena_size) offset = 0;
        uint8_t* wp = arena + offset;
        uint8_t* block = wp;
        // Serialize 4 columns
        for (size_t c = 0; c < NUM_COLS; c++) {
            wp += serialize_one(wp, cols[c][i].ptr, cols[c][i].len);
        }
        offset += total;
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

int main() {
    // Generate data: 4 columns of ~12 byte strings
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

    // Pre-allocate arena (64MB)
    std::vector<uint8_t> arena(64 * 1024 * 1024, 0);

    // Warmup
    volatile uint64_t w = bench_serialize(cols, arena.data(), arena.size());
    (void)w;

    // Measure
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t result = 0;
    for (size_t i = 0; i < NUM_ITERS; i++) {
        result = bench_serialize(cols, arena.data(), arena.size());
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / NUM_ITERS;

    printf("C++ serialize_demo: %.2f ms  checksum=%lu\n", ms, (unsigned long)result);
    return 0;
}
