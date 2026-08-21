/// Step-by-step function benchmark — tests every function in the EmplaceTableWithDecode pipeline.
/// Each function is isolated with real data matching the 4str_0int workload.
///
/// Usage: ./cpp_step_by_step <sel> [iterations]
/// Default: sel=0.1, 10 iterations, 1M rows per iteration

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <random>

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "column_marshaller.h"

static constexpr size_t NUM_STR_COLS = 4;
static constexpr size_t HT_SIZE = 16384;
static constexpr double LOAD_FACTOR = 0.50;
static constexpr size_t NUM_PROBE_ROWS = 1000000;
static constexpr size_t BATCH_SIZE = 410;
static constexpr uint64_t SEED = 42;
static constexpr size_t DEFAULT_ITERS = 10;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}
static inline uint64_t HB(const uint8_t* d, size_t l, uint64_t s) { return XXH3_64bits_withSeed(d, l, s); }

// ═══════════════════════════════════════════════════════════════════
// Data generation
// ═══════════════════════════════════════════════════════════════════

struct TestData {
    std::vector<std::vector<std::vector<uint8_t>>> strCols;
    std::vector<std::vector<taper::VarcharSlice>> slices;
    std::vector<int64_t> hashes;
    std::vector<int64_t> values;
    size_t totalRows;
    size_t numKeys;
    size_t numChunks;
};

static TestData GenData(double sel) {
    size_t numKeys = static_cast<size_t>(HT_SIZE * LOAD_FACTOR);
    std::mt19937_64 rng(SEED);
    TestData d;
    d.numKeys = numKeys;
    d.strCols.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].resize(numKeys);
        for (size_t i = 0; i < numKeys; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.strCols[c][i].assign(s.begin(), s.end());
        }
    }
    std::vector<int64_t> bh(numKeys);
    for (size_t i = 0; i < numKeys; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            h = HB(d.strCols[c][i].data(), d.strCols[c][i].size(), h);
        bh[i] = static_cast<int64_t>(h);
    }

    size_t nH = static_cast<size_t>(NUM_PROBE_ROWS * sel), nM = NUM_PROBE_ROWS - nH;
    std::vector<std::vector<std::vector<uint8_t>>> ps(NUM_STR_COLS);
    std::vector<int64_t> ph;
    for (size_t i = 0; i < nH; i++) { size_t idx = rng() % numKeys; for (size_t c = 0; c < NUM_STR_COLS; c++) ps[c].push_back(d.strCols[c][idx]); ph.push_back(bh[idx]); }
    for (size_t i = 0; i < nM; i++) { uint64_t h = 0; for (size_t c = 0; c < NUM_STR_COLS; c++) { auto s = "miss_" + std::to_string(i) + "_" + std::to_string(c); std::vector<uint8_t> b(s.begin(), s.end()); h = HB(b.data(), b.size(), h); ps[c].push_back(std::move(b)); } ph.push_back(static_cast<int64_t>(h)); }

    std::vector<size_t> ord(NUM_PROBE_ROWS); std::iota(ord.begin(), ord.end(), 0);
    for (size_t i = NUM_PROBE_ROWS - 1; i > 0; i--) std::swap(ord[i], ord[rng() % (i + 1)]);
    for (size_t c = 0; c < NUM_STR_COLS; c++) { auto tmp = std::move(ps[c]); ps[c].resize(NUM_PROBE_ROWS); for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ps[c][i] = std::move(tmp[ord[i]]); }
    { auto tmp = ph; for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ph[i] = tmp[ord[i]]; }

    d.totalRows = numKeys + NUM_PROBE_ROWS;
    for (size_t c = 0; c < NUM_STR_COLS; c++) { d.strCols[c].reserve(d.totalRows); for (auto& v : ps[c]) d.strCols[c].push_back(std::move(v)); }
    d.hashes = bh; d.hashes.insert(d.hashes.end(), ph.begin(), ph.end());
    d.values.resize(d.totalRows); for (size_t i = 0; i < d.totalRows; i++) d.values[i] = i % 1000;

    d.slices.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) { d.slices[c].resize(d.totalRows); for (size_t i = 0; i < d.totalRows; i++) { d.slices[c][i].ptr = d.strCols[c][i].data(); d.slices[c][i].len = d.strCols[c][i].size(); } }

    size_t numMisses = NUM_PROBE_ROWS - static_cast<size_t>(NUM_PROBE_ROWS * sel);
    size_t distinctKeys = numKeys + numMisses;
    size_t minSlots = std::max(static_cast<size_t>(distinctKeys / 0.85), size_t(8));
    d.numChunks = 1; while (d.numChunks * 8 < minSlots) d.numChunks *= 2;

    return d;
}

// ═══════════════════════════════════════════════════════════════════
// Step benchmarks
// ═══════════════════════════════════════════════════════════════════

// 1. Precompute positions: hash & mask
__attribute__((noinline))
static uint64_t BenchPrecomputePositions(const TestData& d) {
    uint32_t mask = static_cast<uint32_t>(d.numChunks - 1);
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint32_t pos = static_cast<uint32_t>(d.hashes[i]) & mask;
        checksum += pos;
    }
    return checksum;
}

// 5. NewRow
__attribute__((noinline))
static uint64_t BenchNewRow(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> ks(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(ks, kinds, 8, pool);
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        char* row = rc.NewRow();
        checksum += reinterpret_cast<uint64_t>(row);
    }
    return checksum;
}

// 7. StoreKeyOneRow (serialize 4 varchar to arena)
__attribute__((noinline))
static uint64_t BenchSerialize(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

// 8. StoreValue<i64>
__attribute__((noinline))
static uint64_t BenchStoreValue(const TestData& d) {
    // Allocate rows first
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> ks(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(ks, kinds, 8, pool);
    std::vector<char*> rows(d.totalRows);
    for (size_t i = 0; i < d.totalRows; i++) rows[i] = rc.NewRow();

    int32_t aggOffset = rc.AggStateOffset();
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        taper::RowContainer::StoreValue<int64_t>(rows[i], aggOffset, d.values[i]);
        checksum += static_cast<uint64_t>(d.values[i]);
    }
    return checksum;
}

// 10. BatchCompareVarchar (compare 4 cols, all equal)
__attribute__((noinline))
static uint64_t BenchCompareVarchar(const TestData& d) {
    // Pre-serialize all rows
    taper::SimpleArenaAllocator pool;
    std::vector<const uint8_t*> blocks(d.totalRows);
    for (size_t i = 0; i < d.totalRows; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        blocks[i] = block;
    }

    // Compare (100% equal)
    uint64_t match_count = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        const uint8_t* pos = blocks[i];
        bool all_match = true;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            if (!taper::CompareVarcharFromRow(pos, d.slices[c][i].ptr, d.slices[c][i].len)) {
                all_match = false; break;
            }
            pos += taper::ComputeVarCharSerializedSize(pos);
        }
        if (all_match) match_count++;
    }
    return match_count;
}

// 12. Accumulate equals: agg += value
__attribute__((noinline))
static uint64_t BenchAccumulate(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> ks(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(ks, kinds, 8, pool);
    int32_t aggOffset = rc.AggStateOffset();
    std::vector<char*> rows(d.totalRows);
    for (size_t i = 0; i < d.totalRows; i++) {
        rows[i] = rc.NewRow();
        taper::RowContainer::StoreValue<int64_t>(rows[i], aggOffset, 0);
    }

    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        int64_t* p = reinterpret_cast<int64_t*>(rows[i % d.numKeys] + aggOffset);
        *p += d.values[i];
        checksum += static_cast<uint64_t>(*p);
    }
    return checksum;
}

// Full pipeline (for comparison)
__attribute__((noinline))
static uint64_t BenchFullPipeline(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<taper::ColumnDesc> cd(NUM_STR_COLS, taper::ColumnDesc::Varchar);
    taper::TaperColumnSerializeHandler t(pool, 8, cd, d.numChunks);
    size_t numBatches = (d.totalRows + BATCH_SIZE - 1) / BATCH_SIZE;
    std::vector<taper::ColumnInput> cols(NUM_STR_COLS);
    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, d.totalRows);
        int32_t batchLen = static_cast<int32_t>(end - start);
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            cols[c] = taper::ColumnInput::MakeVarchar(d.slices[c].data() + start);
        t.EmplaceTableWithDecode(d.hashes.data() + start, batchLen, cols, d.values.data() + start);
    }
    return t.NumGroups();
}

// ═══════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    double sel = 0.1;
    size_t numIters = DEFAULT_ITERS;
    if (argc > 1) sel = std::atof(argv[1]);
    if (argc > 2) numIters = static_cast<size_t>(std::atoi(argv[2]));

    fprintf(stderr, "=== C++ Step-by-Step Bench ===\n");
    fprintf(stderr, "sel=%.1f, iters=%zu\n", sel, numIters);
    fprintf(stderr, "Generating data...\n");
    TestData data = GenData(sel);
    fprintf(stderr, "totalRows=%zu, numKeys=%zu, numChunks=%zu\n\n", data.totalRows, data.numKeys, data.numChunks);

    auto bench = [&](const char* name, uint64_t(*fn)(const TestData&)) {
        volatile uint64_t w = fn(data); (void)w;
        auto t0 = Clock::now();
        volatile uint64_t checksum = 0;
        for (size_t i = 0; i < numIters; i++) checksum = fn(data);
        auto t1 = Clock::now();
        double per_iter = elapsed_ms(t0, t1) / numIters;
        printf("%-30s  %7.2f ms  checksum=%lu\n", name, per_iter, (unsigned long)checksum);
    };

    printf("=== C++ Step-by-Step (sel=%.1f, %zu iters, %zu rows) ===\n", sel, numIters, data.totalRows);
    bench("1. precompute_positions", BenchPrecomputePositions);
    bench("5. new_row", BenchNewRow);
    bench("7. serialize_4str", BenchSerialize);
    bench("8. store_value_i64", BenchStoreValue);
    bench("10. compare_varchar_4col", BenchCompareVarchar);
    bench("12. accumulate_equals", BenchAccumulate);
    bench("FULL: pipeline", BenchFullPipeline);

    return 0;
}
