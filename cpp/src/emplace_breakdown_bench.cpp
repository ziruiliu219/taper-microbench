/// EmplaceBatch Breakdown Bench — times every sub-function in the hash agg pipeline.
///
/// Traces the full EmplaceBatch flow:
///   1. hash_and_position:   Hash(key) + GetChunkPos
///   2. prefetch:            __builtin_prefetch chunk
///   3. load_tags:           chunk->TagsU64()
///   4. match_tag:           PHBitMask::MatchTag (SWAR)
///   5. compare_key_hash:    chunk->keys[slot] == key (int64 compare)
///   6. new_row:             RowContainer::NewRow()
///   7. serialize_key:       StoreKeyOneRow (4 varchar → arena)
///   8. store_value:         StoreValue<int64_t>
///   9. compare_varchar:     CompareVarcharFromRow × 4 cols
///  10. accumulate:          agg += value
///  FULL: pipeline           Complete EmplaceTableWithDecode
///
/// Usage: ./cpp_emplace_breakdown <sel> [iters] [ht_size] [load_factor]

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
#include "taper_hashtable.h"

static constexpr size_t NUM_STR_COLS = 4;
static constexpr size_t NUM_PROBE_ROWS = 1000000;
static constexpr size_t BATCH_SIZE = 410;
static constexpr uint64_t SEED = 42;
static constexpr size_t DEFAULT_ITERS = 10;

static size_t G_HT_SIZE = 16384;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}
static inline uint64_t HB(const uint8_t* d, size_t l, uint64_t s) { return XXH3_64bits_withSeed(d, l, s); }

// ─── Data ────────────────────────────────────────────────────────
struct TestData {
    std::vector<std::vector<std::vector<uint8_t>>> strCols;
    std::vector<std::vector<taper::VarcharSlice>> slices;
    std::vector<int64_t> hashes;
    std::vector<int64_t> values;
    size_t totalRows, numKeys, numChunks;
};

static TestData GenData(double sel) {
    // ─── Two params only: G_HT_SIZE (table slots) + sel (build/probe split) ───
    // Table size fixed, fill to 89% (just under 90% expand threshold), no rehash.
    size_t numChunks = G_HT_SIZE / 8;
    if (numChunks < 1) numChunks = 1;
    size_t nc = 1; while (nc < numChunks) nc <<= 1; numChunks = nc;
    size_t capacity = numChunks * 8;

    // Total distinct keys: use integer arithmetic matching C++ expandThreshold exactly.
    // C++ threshold: expandThreshold_ = capacity * 9 / 10
    // We set distinctKeys = capacity * 9 / 10 - 1, guaranteed < threshold.
    size_t expandThreshold = capacity * 9 / 10;
    size_t distinctKeys = expandThreshold - 1;
    if (distinctKeys < 1) distinctKeys = 1;

    // sel controls how many distinct keys are inserted in the build phase vs probe phase:
    //   numKeys = distinctKeys * sel        (inserted during build, before probe)
    //   probeMisses = distinctKeys - numKeys (new keys encountered during probe)
    //   probeHits = NUM_PROBE_ROWS - probeMisses (probe rows that hit existing keys)
    size_t numKeys = static_cast<size_t>(distinctKeys * sel);
    if (numKeys < 1) numKeys = 1;
    size_t probeMisses = distinctKeys - numKeys;
    size_t probeHits = (NUM_PROBE_ROWS > probeMisses) ? (NUM_PROBE_ROWS - probeMisses) : 0;
    size_t numProbeRows = probeHits + probeMisses;

    fprintf(stderr, "  sizing: numChunks=%zu, capacity=%zu, distinctKeys=%zu\n", numChunks, capacity, distinctKeys);
    fprintf(stderr, "  build: numKeys=%zu (%.1f%% of capacity)\n", numKeys, 100.0*numKeys/capacity);
    fprintf(stderr, "  probe: hits=%zu, misses=%zu, probeRows=%zu\n", probeHits, probeMisses, numProbeRows);
    fprintf(stderr, "  final fill: %zu/%zu = %.1f%% (expand threshold=90%%)\n",
            distinctKeys, capacity, 100.0*distinctKeys/capacity);

    std::mt19937_64 rng(SEED);
    TestData d;
    d.numKeys = numKeys;
    d.numChunks = numChunks;

    // Generate distinct key strings
    d.strCols.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].resize(numKeys);
        for (size_t i = 0; i < numKeys; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.strCols[c][i].assign(s.begin(), s.end());
        }
    }

    // Compute hashes for base keys
    std::vector<int64_t> bh(numKeys);
    for (size_t i = 0; i < numKeys; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            h = HB(d.strCols[c][i].data(), d.strCols[c][i].size(), h);
        bh[i] = static_cast<int64_t>(h);
    }

    // Generate probe rows: probeHits (from existing keys) + probeMisses (new keys)
    std::vector<std::vector<std::vector<uint8_t>>> ps(NUM_STR_COLS);
    std::vector<int64_t> ph;
    for (size_t i = 0; i < probeHits; i++) {
        size_t idx = rng() % numKeys;
        for (size_t c = 0; c < NUM_STR_COLS; c++) ps[c].push_back(d.strCols[c][idx]);
        ph.push_back(bh[idx]);
    }
    for (size_t i = 0; i < probeMisses; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            auto s = "miss_" + std::to_string(i) + "_" + std::to_string(c);
            std::vector<uint8_t> b(s.begin(), s.end());
            h = HB(b.data(), b.size(), h);
            ps[c].push_back(std::move(b));
        }
        ph.push_back(static_cast<int64_t>(h));
    }

    // Shuffle probe rows
    std::vector<size_t> ord(numProbeRows);
    std::iota(ord.begin(), ord.end(), 0);
    for (size_t i = numProbeRows - 1; i > 0; i--) std::swap(ord[i], ord[rng() % (i + 1)]);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        auto tmp = std::move(ps[c]); ps[c].resize(numProbeRows);
        for (size_t i = 0; i < numProbeRows; i++) ps[c][i] = std::move(tmp[ord[i]]);
    }
    { auto tmp = ph; for (size_t i = 0; i < numProbeRows; i++) ph[i] = tmp[ord[i]]; }

    // Combine: base keys first, then probe rows
    d.totalRows = numKeys + numProbeRows;
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].reserve(d.totalRows);
        for (auto& v : ps[c]) d.strCols[c].push_back(std::move(v));
    }
    d.hashes = bh;
    d.hashes.insert(d.hashes.end(), ph.begin(), ph.end());
    d.values.resize(d.totalRows);
    for (size_t i = 0; i < d.totalRows; i++) d.values[i] = i % 1000;

    // Build slices
    d.slices.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.slices[c].resize(d.totalRows);
        for (size_t i = 0; i < d.totalRows; i++) {
            d.slices[c][i].ptr = d.strCols[c][i].data();
            d.slices[c][i].len = d.strCols[c][i].size();
        }
    }
    return d;
}

// ─── Individual Steps (using real taper API) ────────────────────

// 1. Hash & position (uses real TaperFlatHashTable::BenchHash + BenchGetChunkPos)
__attribute__((noinline))
static uint64_t BenchHashAndPosition(const TestData& d) {
    taper::TaperFlatHashTable table(d.numChunks);
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint64_t hv = table.BenchHash(d.hashes[i]);
        uint32_t pos = table.BenchGetChunkPos(hv);
        checksum += pos;
    }
    return checksum;
}

// 3. Load tags (uses real ChunkAt->TagsU64)
__attribute__((noinline))
static uint64_t BenchLoadTags(const TestData& d) {
    taper::TaperFlatHashTable table(d.numChunks);
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint64_t hv = table.BenchHash(d.hashes[i]);
        uint32_t pos = table.BenchGetChunkPos(hv);
        uint64_t t = table.ChunkAt(pos)->TagsU64();
        checksum += t;
    }
    return checksum;
}

// 4. Match tag (uses real PHBitMask::MatchTag on real chunks)
__attribute__((noinline))
static uint64_t BenchMatchTag(const TestData& d) {
    taper::TaperFlatHashTable table(d.numChunks);
    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint64_t hv = table.BenchHash(d.hashes[i]);
        uint32_t pos = table.BenchGetChunkPos(hv);
        uint8_t tagHash = (hv >> 16) & 0x7F;
        uint64_t tags = table.ChunkAt(pos)->TagsU64();
        for (auto it = taper::PHBitMask::MatchTag(tags, tagHash); it; ++it) {
            checksum += *it;
        }
    }
    return checksum;
}

// 5. Compare key (uses real chunk->keys[slot] via ChunkAt, on a pre-built hashmap)
__attribute__((noinline))
static uint64_t BenchCompareKeyHash(const TestData& d) {
    // Build a real hashmap first
    taper::TaperFlatHashTable table(d.numChunks);
    table.EmplaceBatch(d.hashes.data(), static_cast<int32_t>(d.numKeys),
        [](int32_t) { return false; },
        [](uint32_t, char*) {},
        [](uint32_t, char*, bool) {});
    // Now probe: for each row, find the chunk and compare key
    uint64_t matchCount = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint64_t hv = table.BenchHash(d.hashes[i]);
        uint32_t pos = table.BenchGetChunkPos(hv);
        uint8_t tagHash = (hv >> 16) & 0x7F;
        const auto* chunk = table.ChunkAt(pos);
        uint64_t tags = chunk->TagsU64();
        for (auto it = taper::PHBitMask::MatchTag(tags, tagHash); it; ++it) {
            uint32_t slot = *it;
            if (chunk->keys[slot] == static_cast<uint64_t>(d.hashes[i])) {
                matchCount++;
                break;
            }
        }
    }
    return matchCount;
}

// 6. NewRow
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

// 7. Serialize key (4 varchar)
__attribute__((noinline))
static uint64_t BenchSerializeKey(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    uint64_t checksum = 0;
    volatile size_t numCols = NUM_STR_COLS; // prevent unrolling (like Rust black_box)
    for (size_t i = 0; i < d.totalRows; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < numCols; c++) {
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        }
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < numCols; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        checksum += reinterpret_cast<uint64_t>(block);
    }
    return checksum;
}

// 8. Store value
__attribute__((noinline))
static uint64_t BenchStoreValue(const TestData& d) {
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

// 9. Compare varchar (4 cols, pre-serialized, 100% match)
__attribute__((noinline))
static uint64_t BenchCompareVarchar(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    volatile size_t numCols = NUM_STR_COLS;
    std::vector<const uint8_t*> blocks(d.totalRows);
    for (size_t i = 0; i < d.totalRows; i++) {
        size_t totalSize = 0;
        for (size_t c = 0; c < numCols; c++) {
            totalSize += 1 + taper::ComputeRowLenSize(d.slices[c][i].len) + d.slices[c][i].len;
        }
        uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
        uint8_t* wp = block;
        for (size_t c = 0; c < numCols; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        blocks[i] = block;
    }
    uint64_t match_count = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
        const uint8_t* pos = blocks[i];
        bool all_match = true;
        for (size_t c = 0; c < numCols; c++) {
            if (!taper::CompareVarcharFromRow(pos, d.slices[c][i].ptr, d.slices[c][i].len)) {
                all_match = false; break;
            }
            pos += taper::ComputeVarCharSerializedSize(pos);
        }
        if (all_match) match_count++;
    }
    return match_count;
}

// 10. Accumulate
__attribute__((noinline))
static uint64_t BenchAccumulate(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> ks(4, 0);
    std::vector<taper::ColumnKind> kinds(4, taper::ColumnKind::Varchar);
    taper::RowContainer rc(ks, kinds, 8, pool);
    int32_t aggOffset = rc.AggStateOffset();
    std::vector<char*> rows(d.numKeys);
    for (size_t i = 0; i < d.numKeys; i++) {
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

// FULL pipeline
__attribute__((noinline, flatten))
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

// ─── Main ────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    double sel = 0.1;
    size_t numIters = DEFAULT_ITERS;
    if (argc > 1) sel = std::atof(argv[1]);
    if (argc > 2) numIters = static_cast<size_t>(std::atoi(argv[2]));
    if (argc > 3) G_HT_SIZE = static_cast<size_t>(std::atoi(argv[3]));

    fprintf(stderr, "=== C++ EmplaceBatch Breakdown ===\n");
    fprintf(stderr, "sel=%.2f, iters=%zu, ht=%zu\n", sel, numIters, G_HT_SIZE);
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

    // Optional stage filter: argv[4] = "5" or "7" or "9" or "5,7,9" or "FULL"
    std::string stageFilter = "";
    if (argc > 4) stageFilter = argv[4];

    auto shouldRun = [&](const char* name) {
        if (stageFilter.empty()) return true;
        return stageFilter.find(std::string(name).substr(0, stageFilter.size())) != std::string::npos
            || stageFilter.find(name) != std::string::npos;
    };

    printf("=== C++ EmplaceBatch Breakdown (ht=%zu, sel=%.2f, %zu iters, %zu rows) ===\n",
           G_HT_SIZE, sel, numIters, data.totalRows);
    if (shouldRun("1")) bench("1. hash_and_position", BenchHashAndPosition);
    if (shouldRun("3")) bench("3. load_tags", BenchLoadTags);
    if (shouldRun("4")) bench("4. match_tag_swar", BenchMatchTag);
    if (shouldRun("5")) bench("5. compare_key_hash", BenchCompareKeyHash);
    if (shouldRun("6")) bench("6. new_row", BenchNewRow);
    if (shouldRun("7")) bench("7. serialize_key_4col", BenchSerializeKey);
    if (shouldRun("8")) bench("8. store_value_i64", BenchStoreValue);
    if (shouldRun("9")) bench("9. compare_varchar_4col", BenchCompareVarchar);
    if (shouldRun("10")) bench("10. accumulate", BenchAccumulate);
    if (shouldRun("FULL") || shouldRun("11")) bench("FULL: pipeline", BenchFullPipeline);

    return 0;
}
