/// Real batch pipeline micro benchmark — incrementally adds layers to find where C++ loses to Rust.
///
/// Case A: hashmap probe only (trivial init/update, no StoreKey, no compare)
/// Case B: hashmap + real GetUnequalsNumWithDecode batch compare
/// Case C: hashmap + batch compare + NewRow (no varchar serialize)
/// Case D: hashmap + batch compare + NewRow + StoreKeyOneRow (full pipeline)
///
/// Usage: ./cpp_micro_pipeline <sel> [iterations]
/// Default: sel=0.1, 10 iterations

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
static constexpr size_t NUM_INT_COLS = 0;
static constexpr size_t HT_SIZE = 16384;
static constexpr double LOAD_FACTOR = 0.50;
static constexpr size_t NUM_PROBE_ROWS = 1000000;
static constexpr size_t BATCH_SIZE = 410;
static constexpr uint64_t SEED = 42;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

// ─── Data gen (same as profile_taper_standalone) ─────────────────────────
static inline uint64_t HB(const uint8_t* d, size_t l, uint64_t s) { return XXH3_64bits_withSeed(d, l, s); }

struct BenchData {
    std::vector<std::vector<std::vector<uint8_t>>> strCols;
    std::vector<int64_t> hashes, values;
    std::vector<std::vector<taper::VarcharSlice>> strSlices;
    size_t totalRows;
};

static BenchData GenData(size_t nKeys, double sel) {
    std::mt19937_64 rng(SEED);
    BenchData d;
    d.strCols.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].resize(nKeys);
        for (size_t i = 0; i < nKeys; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.strCols[c][i].assign(s.begin(), s.end());
        }
    }
    std::vector<int64_t> bh(nKeys);
    for (size_t i = 0; i < nKeys; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            h = HB(d.strCols[c][i].data(), d.strCols[c][i].size(), h);
        bh[i] = static_cast<int64_t>(h);
    }
    std::vector<int64_t> bv(nKeys);
    for (size_t i = 0; i < nKeys; i++) bv[i] = i % 1000;

    size_t nH = static_cast<size_t>(NUM_PROBE_ROWS * sel), nM = NUM_PROBE_ROWS - nH;
    std::vector<std::vector<std::vector<uint8_t>>> ps(NUM_STR_COLS);
    for (auto& v : ps) v.reserve(NUM_PROBE_ROWS);
    std::vector<int64_t> ph; ph.reserve(NUM_PROBE_ROWS);
    for (size_t i = 0; i < nH; i++) { size_t idx = rng() % nKeys; for (size_t c = 0; c < NUM_STR_COLS; c++) ps[c].push_back(d.strCols[c][idx]); ph.push_back(bh[idx]); }
    for (size_t i = 0; i < nM; i++) { uint64_t h = 0; for (size_t c = 0; c < NUM_STR_COLS; c++) { auto s = "miss_" + std::to_string(i) + "_" + std::to_string(c); std::vector<uint8_t> b(s.begin(), s.end()); h = HB(b.data(), b.size(), h); ps[c].push_back(std::move(b)); } ph.push_back(static_cast<int64_t>(h)); }
    std::vector<size_t> ord(NUM_PROBE_ROWS); std::iota(ord.begin(), ord.end(), 0);
    for (size_t i = NUM_PROBE_ROWS - 1; i > 0; i--) std::swap(ord[i], ord[rng() % (i + 1)]);
    for (size_t c = 0; c < NUM_STR_COLS; c++) { auto tmp = std::move(ps[c]); ps[c].resize(NUM_PROBE_ROWS); for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ps[c][i] = std::move(tmp[ord[i]]); }
    { auto tmp = ph; for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ph[i] = tmp[ord[i]]; }
    std::vector<int64_t> pv(NUM_PROBE_ROWS); for (size_t i = 0; i < NUM_PROBE_ROWS; i++) pv[i] = i % 1000;

    d.totalRows = nKeys + NUM_PROBE_ROWS;
    for (size_t c = 0; c < NUM_STR_COLS; c++) { d.strCols[c].reserve(d.totalRows); for (auto& v : ps[c]) d.strCols[c].push_back(std::move(v)); }
    d.hashes = bh; d.hashes.insert(d.hashes.end(), ph.begin(), ph.end());
    d.values = bv; d.values.insert(d.values.end(), pv.begin(), pv.end());
    d.strSlices.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) { d.strSlices[c].resize(d.totalRows); for (size_t i = 0; i < d.totalRows; i++) { d.strSlices[c][i].ptr = d.strCols[c][i].data(); d.strSlices[c][i].len = d.strCols[c][i].size(); } }
    return d;
}

// ─── Case C2: HashMap + NewRow + StoreKeyOneRow (serialize, no batch compare) ─
__attribute__((noinline, flatten))
static size_t RunHashmapPlusSerialize(const BenchData& d, size_t numChunks) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);
    taper::TaperFlatHashTable table(numChunks);
    int32_t aggOffset = rc.AggStateOffset();
    uint64_t sum = 0;

    // Process in batches (same as real pipeline)
    size_t numBatches = (d.totalRows + BATCH_SIZE - 1) / BATCH_SIZE;
    std::vector<taper::ColumnInput> cols(NUM_STR_COLS);

    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, d.totalRows);
        int32_t batchLen = static_cast<int32_t>(end - start);

        for (size_t c = 0; c < NUM_STR_COLS; c++)
            cols[c] = taper::ColumnInput::MakeVarchar(d.strSlices[c].data() + start);

        const taper::ColumnInput* colsPtr = cols.data();
        taper::RowContainer* rcPtr = &rc;

        table.EmplaceBatch(d.hashes.data() + start, batchLen,
            [](int32_t) { return false; },
            [&](uint32_t rowIdx, char* data) {
                char* row = rcPtr->NewRow();
                // SetRowPtr
                uint64_t ptr = reinterpret_cast<uint64_t>(row);
                memcpy(data, &ptr, taper::ROW_PTR_SIZE);
                // StoreKeyOneRow — serialize 4 varchars into merged block
                size_t totalSize = 0;
                for (size_t c = 0; c < NUM_STR_COLS; c++) {
                    totalSize += 1 + taper::ComputeRowLenSize(colsPtr[c].vcSlices[rowIdx].len) + colsPtr[c].vcSlices[rowIdx].len;
                }
                uint8_t* block = rcPtr->ArenaAlloc(totalSize);
                uint8_t* wp = block;
                for (size_t c = 0; c < NUM_STR_COLS; c++) {
                    wp += taper::SerializeVarcharToBuffer(wp, colsPtr[c].vcSlices[rowIdx].ptr, colsPtr[c].vcSlices[rowIdx].len);
                }
                memcpy(row, &block, sizeof(block)); // store pointer at offset 0 (varchar slot col)
                taper::RowContainer::StoreValue<int64_t>(row, aggOffset, d.values[start + rowIdx]);
            },
            [&sum](uint32_t, char*, bool isNew) { if (!isNew) sum++; }
        );
    }
    return table.Size() + sum;
}

// ─── Case D: Full pipeline (same as profile_taper_standalone) ─────────────
__attribute__((noinline))
static size_t RunFullPipeline(const BenchData& d, size_t numChunks) {
    taper::SimpleArenaAllocator pool;
    std::vector<taper::ColumnDesc> cd(NUM_STR_COLS, taper::ColumnDesc::Varchar);
    taper::TaperColumnSerializeHandler t(pool, 8, cd, numChunks);
    size_t numBatches = (d.totalRows + BATCH_SIZE - 1) / BATCH_SIZE;
    std::vector<taper::ColumnInput> cols(NUM_STR_COLS);
    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, d.totalRows);
        int32_t batchLen = static_cast<int32_t>(end - start);
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            cols[c] = taper::ColumnInput::MakeVarchar(d.strSlices[c].data() + start);
        t.EmplaceTableWithDecode(d.hashes.data() + start, batchLen, cols, d.values.data() + start);
    }
    return t.NumGroups();
}

// ─── Case A: HashMap probe only (trivial callbacks) ───────────────────────
__attribute__((noinline))
static size_t RunHashmapOnly(const BenchData& d, size_t numChunks) {
    taper::TaperFlatHashTable table(numChunks);
    uint64_t sum = 0;
    table.EmplaceBatch(d.hashes.data(), static_cast<int32_t>(d.totalRows),
        [](int32_t) { return false; },
        [](uint32_t, char* data) { memset(data, 0x42, taper::ROW_PTR_SIZE); },
        [&sum](uint32_t, char*, bool isNew) { if (!isNew) sum++; }
    );
    return table.Size() + sum;
}

// ─── Case C: HashMap + NewRow (no serialize) ──────────────────────────────
__attribute__((noinline))
static size_t RunHashmapPlusNewRow(const BenchData& d, size_t numChunks) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);
    taper::TaperFlatHashTable table(numChunks);
    int32_t aggOffset = rc.AggStateOffset();
    uint64_t sum = 0;

    table.EmplaceBatch(d.hashes.data(), static_cast<int32_t>(d.totalRows),
        [](int32_t) { return false; },
        [&](uint32_t rowIdx, char* data) {
            char* row = rc.NewRow();
            // Just write pointer + agg, no varchar serialize
            uint64_t ptr = reinterpret_cast<uint64_t>(row);
            memcpy(data, &ptr, taper::ROW_PTR_SIZE);
            taper::RowContainer::StoreValue<int64_t>(row, aggOffset, d.values[rowIdx]);
        },
        [&sum](uint32_t, char*, bool isNew) { if (!isNew) sum++; }
    );
    return table.Size() + sum;
}

// ═══════════════════════════════════════════════════════════════════
int main(int argc, char** argv) {
    double sel = 0.1;
    size_t numIters = 10;
    const char* stage = nullptr; // null = run all

    for (int i = 1; i < argc; i++) {
        if (i == 1) sel = std::atof(argv[i]);
        else if (i == 2) {
            // Check if it's a stage name or iteration count
            if (argv[i][0] >= 'A' && argv[i][0] <= 'Z') {
                stage = argv[i];
            } else {
                numIters = static_cast<size_t>(std::atoi(argv[i]));
            }
        }
        else if (i == 3) numIters = static_cast<size_t>(std::atoi(argv[i]));
    }

    size_t numKeys = static_cast<size_t>(HT_SIZE * LOAD_FACTOR);
    size_t numMisses = NUM_PROBE_ROWS - static_cast<size_t>(NUM_PROBE_ROWS * sel);
    size_t distinctKeys = numKeys + numMisses;
    size_t minSlots = std::max(static_cast<size_t>(distinctKeys / 0.85), size_t(8));
    size_t numChunks = 1;
    while (numChunks * 8 < minSlots) numChunks *= 2;

    fprintf(stderr, "=== C++ Pipeline Micro Bench ===\n");
    fprintf(stderr, "sel=%.1f, iters=%zu, totalRows=%zu, numChunks=%zu\n", sel, numIters, numKeys + NUM_PROBE_ROWS, numChunks);
    if (stage) fprintf(stderr, "Stage filter: %s\n", stage);
    fprintf(stderr, "\nGenerating data...\n");
    BenchData data = GenData(numKeys, sel);
    fprintf(stderr, "Done.\n\n");

    auto bench = [&](const char* name, const char* tag, size_t(*fn)(const BenchData&, size_t)) {
        if (stage && strcmp(stage, tag) != 0) return;
        volatile size_t w = fn(data, numChunks); (void)w;
        auto t0 = Clock::now();
        volatile size_t result = 0;
        for (size_t i = 0; i < numIters; i++) result = fn(data, numChunks);
        auto t1 = Clock::now();
        double per_iter = elapsed_ms(t0, t1) / numIters;
        printf("%-30s  per_iter=%7.2f ms  result=%zu\n", name, per_iter, (size_t)result);
    };

    printf("=== Results (sel=%.1f, %zu iters) ===\n", sel, numIters);
    bench("A: hashmap_only", "A", RunHashmapOnly);
    bench("C: hashmap+newrow", "C", RunHashmapPlusNewRow);
    bench("C2: hashmap+newrow+serialize", "C2", RunHashmapPlusSerialize);
    bench("D: full_pipeline", "D", RunFullPipeline);

    return 0;
}
