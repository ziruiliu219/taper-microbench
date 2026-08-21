/// Individual function micro benchmarks — prove each function is equally fast in C++ and Rust.
///
/// Usage: ./cpp_micro_functions [iterations]
/// Default: 10 iterations, 1,000,000 operations each

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>
#include <random>

#include "column_marshaller.h"

static constexpr size_t NUM_OPS = 1000000;
static constexpr size_t DEFAULT_ITERS = 10;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ─── 1. arena_alloc_only ─────────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunArenaAllocOnly() {
    taper::SimpleArenaAllocator pool;
    uint64_t checksum = 0;
    for (size_t i = 0; i < NUM_OPS; i++) {
        uint8_t* p = pool.Allocate(50);
        checksum += reinterpret_cast<uint64_t>(p);
    }
    return checksum;
}

// ─── 2. new_row_only ─────────────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunNewRowOnly() {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes = {0, 0, 0, 0};
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);
    uint64_t checksum = 0;
    for (size_t i = 0; i < NUM_OPS; i++) {
        char* row = rc.NewRow();
        checksum += reinterpret_cast<uint64_t>(row);
    }
    return checksum;
}

// ─── 3. serialize_4str_only ──────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunSerialize4strOnly(const std::vector<std::vector<taper::VarcharSlice>>& slices) {
    taper::SimpleArenaAllocator pool;
    uint64_t checksum = 0;
    for (size_t i = 0; i < NUM_OPS; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < 4; c++) {
            totalSize += 1 + taper::ComputeRowLenSize(slices[c][i].len) + slices[c][i].len;
        }
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < 4; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, slices[c][i].ptr, slices[c][i].len);
        }
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

// ─── 4. compare_4str_only ────────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunCompare4strOnly(
    const std::vector<const uint8_t*>& arenaBlocks,
    const std::vector<std::vector<taper::VarcharSlice>>& slices)
{
    uint64_t match_count = 0;
    for (size_t i = 0; i < NUM_OPS; i++) {
        const uint8_t* pos = arenaBlocks[i];
        bool all_match = true;
        for (size_t c = 0; c < 4; c++) {
            if (!taper::CompareVarcharFromRow(pos, slices[c][i].ptr, slices[c][i].len)) {
                all_match = false;
                break;
            }
            pos += taper::ComputeVarCharSerializedSize(pos);
        }
        if (all_match) match_count++;
    }
    return match_count;
}

// ─── 5. hashmap_probe_only ───────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunHashmapProbeOnly(const std::vector<int64_t>& hashes, size_t numChunks) {
    taper::TaperFlatHashTable table(numChunks);
    uint64_t sum = 0;
    table.EmplaceBatch(hashes.data(), static_cast<int32_t>(hashes.size()),
        [](int32_t) { return false; },
        [](uint32_t, char* data) { memset(data, 0x42, taper::ROW_PTR_SIZE); },
        [&sum](uint32_t, char*, bool isNew) { if (!isNew) sum++; }
    );
    return table.Size() + sum;
}

// ─── 6. setrowptr_only ──────────────────────────────────────────────────
__attribute__((noinline))
static uint64_t RunSetGetRowPtrOnly() {
    char buf[6];
    uint64_t checksum = 0;
    for (size_t i = 0; i < NUM_OPS; i++) {
        uint8_t* ptr = reinterpret_cast<uint8_t*>(0x7FFF00000000ULL + i * 41);
        taper::SetRowPtr(buf, ptr);
        uint8_t* got = taper::GetRowPtr(buf);
        checksum += reinterpret_cast<uint64_t>(got);
    }
    return checksum;
}

// ═══════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    size_t numIters = DEFAULT_ITERS;
    if (argc > 1) numIters = static_cast<size_t>(atoi(argv[1]));

    // Generate test data
    fprintf(stderr, "Generating data...\n");
    std::vector<std::vector<std::vector<uint8_t>>> strCols(4);
    std::vector<std::vector<taper::VarcharSlice>> slices(4);
    for (size_t c = 0; c < 4; c++) {
        strCols[c].resize(NUM_OPS);
        slices[c].resize(NUM_OPS);
        for (size_t i = 0; i < NUM_OPS; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            strCols[c][i].assign(s.begin(), s.end());
            slices[c][i].ptr = strCols[c][i].data();
            slices[c][i].len = strCols[c][i].size();
        }
    }

    // Pre-serialize for compare bench
    taper::SimpleArenaAllocator setupPool;
    std::vector<const uint8_t*> arenaBlocks(NUM_OPS);
    for (size_t i = 0; i < NUM_OPS; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < 4; c++)
            totalSize += 1 + taper::ComputeRowLenSize(slices[c][i].len) + slices[c][i].len;
        uint8_t* block = setupPool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < 4; c++)
            wp += taper::SerializeVarcharToBuffer(wp, slices[c][i].ptr, slices[c][i].len);
        arenaBlocks[i] = block;
    }

    // Hashes for probe bench
    std::mt19937_64 rng(42);
    std::vector<int64_t> hashes(NUM_OPS);
    for (size_t i = 0; i < NUM_OPS; i++) hashes[i] = static_cast<int64_t>(rng());
    size_t numChunks = 1;
    while (numChunks * 8 < NUM_OPS) numChunks *= 2;

    fprintf(stderr, "Done. Running %zu iters...\n\n", numIters);

    auto bench = [&](const char* name, auto fn) {
        volatile uint64_t w = fn(); (void)w; // warmup
        auto t0 = Clock::now();
        volatile uint64_t checksum = 0;
        for (size_t i = 0; i < numIters; i++) checksum = fn();
        auto t1 = Clock::now();
        double per_iter = elapsed_ms(t0, t1) / numIters;
        printf("%-25s  per_iter=%7.3f ms  checksum=%lu\n", name, per_iter, (unsigned long)checksum);
    };

    printf("=== C++ Individual Function Bench (%zu ops, %zu iters) ===\n", NUM_OPS, numIters);
    bench("1. arena_alloc_only", RunArenaAllocOnly);
    bench("2. new_row_only", RunNewRowOnly);
    bench("3. serialize_4str_only", [&]() { return RunSerialize4strOnly(slices); });
    bench("4. compare_4str_only", [&]() { return RunCompare4strOnly(arenaBlocks, slices); });
    bench("5. hashmap_probe_only", [&]() { return RunHashmapProbeOnly(hashes, numChunks); });
    bench("6. setrowptr_only", RunSetGetRowPtrOnly);

    return 0;
}
