/// Micro benchmarks to isolate C++ vs Rust performance in row/string paths.
///
/// Usage: ./cpp_micro_cases [--case serialize_only|compare_only|hashmap_plus_empty_row] [iterations]
///
/// Default: all cases, 10 iterations

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>
#include <random>

#include "column_marshaller.h"

static constexpr size_t NUM_ROWS = 1000000;
static constexpr size_t NUM_STR_COLS = 4;
static constexpr size_t DEFAULT_ITERS = 10;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ═══════════════════════════════════════════════════════════════════
// Data generation: 4 varchar columns, strings ~10-13 bytes
// ═══════════════════════════════════════════════════════════════════

struct TestData {
    std::vector<std::vector<std::vector<uint8_t>>> strCols; // [col][row] -> bytes
    std::vector<std::vector<taper::VarcharSlice>> slices;   // [col][row] -> {ptr, len}
    std::vector<int64_t> hashes;
};

static TestData GenTestData() {
    TestData d;
    d.strCols.resize(NUM_STR_COLS);
    d.slices.resize(NUM_STR_COLS);
    d.hashes.resize(NUM_ROWS);

    std::mt19937_64 rng(42);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].resize(NUM_ROWS);
        d.slices[c].resize(NUM_ROWS);
        for (size_t i = 0; i < NUM_ROWS; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.strCols[c][i].assign(s.begin(), s.end());
            d.slices[c][i].ptr = d.strCols[c][i].data();
            d.slices[c][i].len = d.strCols[c][i].size();
        }
    }
    for (size_t i = 0; i < NUM_ROWS; i++) {
        d.hashes[i] = static_cast<int64_t>(rng());
    }
    return d;
}

// ═══════════════════════════════════════════════════════════════════
// Case 1: serialize_only
// No hashmap. For each row, allocate a RowContainer row and serialize 4 varchars.
// ═══════════════════════════════════════════════════════════════════

__attribute__((noinline))
static uint64_t RunSerializeOnly(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes = {0, 0, 0, 0};
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);

    uint64_t checksum = 0;
    int32_t aggOffset = rc.AggStateOffset();
    int32_t varcharSlotOffset = rc.ColumnAt(0).Offset();

    for (size_t i = 0; i < NUM_ROWS; i++) {
        char* row = rc.NewRow();

        // Serialize 4 varchars into one merged block (same as StoreKeyOneRow merged path)
        size_t totalSize = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        }
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        // Store pointer in slot column
        memcpy(row + varcharSlotOffset, &block, sizeof(block));

        // Store agg value
        int64_t val = static_cast<int64_t>(i % 1000);
        taper::RowContainer::StoreValue<int64_t>(row, aggOffset, val);
        checksum += static_cast<uint64_t>(val);
    }

    return checksum + static_cast<uint64_t>(rc.NumRows());
}

// ═══════════════════════════════════════════════════════════════════
// Case 2: compare_only
// Pre-serialize all rows, then compare each row against its own data (100% equal).
// ═══════════════════════════════════════════════════════════════════

__attribute__((noinline))
static uint64_t RunCompareOnly(const TestData& d) {
    // First, serialize all rows
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes = {0, 0, 0, 0};
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);

    std::vector<const uint8_t*> arenaBlocks(NUM_ROWS);
    int32_t varcharSlotOffset = rc.ColumnAt(0).Offset();

    for (size_t i = 0; i < NUM_ROWS; i++) {
        char* row = rc.NewRow();
        size_t totalSize = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        }
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        memcpy(row + varcharSlotOffset, &block, sizeof(block));
        arenaBlocks[i] = block;
    }

    // Now compare: walk each serialized block vs original data (100% equal)
    uint64_t match_count = 0;
    for (size_t i = 0; i < NUM_ROWS; i++) {
        const uint8_t* pos = arenaBlocks[i];
        bool all_match = true;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            if (!taper::CompareVarcharFromRow(pos, d.slices[c][i].ptr, d.slices[c][i].len)) {
                all_match = false;
                break;
            }
            pos += taper::ComputeVarCharSerializedSize(pos);
        }
        if (all_match) match_count++;
    }

    return match_count;
}

// ═══════════════════════════════════════════════════════════════════
// Case 3: hashmap_plus_empty_row
// Real TaperHashMap with EmplaceBatch. New key = allocate fixed-size row + write checksum.
// No varchar serialization.
// ═══════════════════════════════════════════════════════════════════

__attribute__((noinline))
static uint64_t RunHashmapPlusEmptyRow(const TestData& d) {
    // Compute numChunks for NUM_ROWS distinct keys
    size_t numChunks = 1;
    while (numChunks * 8 < static_cast<size_t>(NUM_ROWS / 0.85)) numChunks *= 2;

    taper::TaperFlatHashTable table(numChunks);
    taper::SimpleArenaAllocator pool;

    // Row = 41 bytes (same as 4str_0int): 4 * 8B ptr slots + 1B null + 8B agg = 41
    static constexpr size_t ROW_SIZE = 41;
    static constexpr int32_t AGG_OFFSET = 33; // 32 (4 ptr slots) + 1 (null byte)
    static constexpr int32_t BATCH_BLOCK = 1024;

    char* batchPtr = nullptr;
    int32_t batchRemaining = 0;
    uint64_t checksum = 0;

    table.EmplaceBatch(d.hashes.data(), static_cast<int32_t>(NUM_ROWS),
        [](int32_t) { return false; },
        [&](uint32_t rowIdx, char* data) {
            // Allocate row (same logic as RowContainer::NewRow)
            if (batchRemaining <= 0) {
                size_t sz = ROW_SIZE * BATCH_BLOCK;
                batchPtr = reinterpret_cast<char*>(pool.Allocate(static_cast<int64_t>(sz)));
                memset(batchPtr, 0, sz);
                batchRemaining = BATCH_BLOCK;
            }
            char* row = batchPtr;
            batchPtr += ROW_SIZE;
            batchRemaining--;

            // Write agg value (no varchar serialization)
            int64_t val = static_cast<int64_t>(rowIdx % 1000);
            memcpy(row + AGG_OFFSET, &val, sizeof(val));

            // Store row pointer in slot
            uint64_t ptr = reinterpret_cast<uint64_t>(row);
            memcpy(data, &ptr, taper::ROW_PTR_SIZE);
        },
        [&checksum](uint32_t rowIdx, char* data, bool isNew) {
            if (!isNew) {
                // Read agg and accumulate
                uint64_t ptr = 0;
                memcpy(&ptr, data, taper::ROW_PTR_SIZE);
                char* row = reinterpret_cast<char*>(ptr);
                int64_t val;
                memcpy(&val, row + AGG_OFFSET, sizeof(val));
                checksum += static_cast<uint64_t>(val);
            }
        }
    );

    return table.Size() + checksum;
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    size_t numIters = DEFAULT_ITERS;
    const char* caseFilter = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            caseFilter = argv[++i];
        } else {
            numIters = static_cast<size_t>(atoi(argv[i]));
        }
    }

    fprintf(stderr, "Generating test data (%zu rows, %zu str cols)...\n", NUM_ROWS, NUM_STR_COLS);
    TestData data = GenTestData();
    fprintf(stderr, "Done.\n\n");

    auto runCase = [&](const char* name, uint64_t(*fn)(const TestData&)) {
        if (caseFilter && strcmp(caseFilter, name) != 0) return;

        fprintf(stderr, "--- %s (warmup) ---\n", name);
        volatile uint64_t w = fn(data);
        (void)w;

        fprintf(stderr, "--- %s (%zu iters) ---\n", name, numIters);
        auto t0 = Clock::now();
        volatile uint64_t checksum = 0;
        for (size_t iter = 0; iter < numIters; iter++) {
            checksum = fn(data);
        }
        auto t1 = Clock::now();
        double total_ms = elapsed_ms(t0, t1);
        double per_iter_ms = total_ms / numIters;

        printf("%-25s  total=%8.1f ms  per_iter=%7.3f ms  checksum=%lu\n",
            name, total_ms, per_iter_ms, (unsigned long)checksum);
    };

    // Case 3 needs a wrapper since it doesn't use TestData& directly for compare
    auto runCase3 = [&]() {
        if (caseFilter && strcmp(caseFilter, "hashmap_plus_empty_row") != 0) return;

        fprintf(stderr, "--- hashmap_plus_empty_row (warmup) ---\n");
        volatile uint64_t w = RunHashmapPlusEmptyRow(data);
        (void)w;

        fprintf(stderr, "--- hashmap_plus_empty_row (%zu iters) ---\n", numIters);
        auto t0 = Clock::now();
        volatile uint64_t checksum = 0;
        for (size_t iter = 0; iter < numIters; iter++) {
            checksum = RunHashmapPlusEmptyRow(data);
        }
        auto t1 = Clock::now();
        double total_ms = elapsed_ms(t0, t1);
        double per_iter_ms = total_ms / numIters;

        printf("%-25s  total=%8.1f ms  per_iter=%7.3f ms  checksum=%lu\n",
            "hashmap_plus_empty_row", total_ms, per_iter_ms, (unsigned long)checksum);
    };

    printf("=== C++ Micro Cases (rows=%zu, iters=%zu) ===\n", NUM_ROWS, numIters);
    runCase("serialize_only", RunSerializeOnly);
    runCase("compare_only", RunCompareOnly);
    runCase3();

    return 0;
}
