/// EmplaceBatch Breakdown Bench — times every sub-function in the hash agg pipeline.
///
/// Traces the full EmplaceBatch flow:
///   1. hash_and_position:   Hash(key) + GetChunkPos
///   2. prefetch:            __builtin_prefetch chunk
///   3. load_tags:           chunk->TagsU64()
///   4. match_tag:           PHBitMask::MatchTag (SWAR)
///   5. compare_key_hash:    chunk->keys[slot] == key (int64 compare)
///   6. serialize_key:       StoreKeyOneRow (4 varchar → arena)
///   7. compare_varchar:     CompareVarcharFromRow × 4 cols
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
#include <memory>
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
    std::vector<taper::VarcharSlice> flatSlices; // flatSlices[i * NUM_STR_COLS + c]
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
    // Build flat slices: row-major [i * NUM_STR_COLS + c]
    d.flatSlices.resize(d.totalRows * NUM_STR_COLS);
    for (size_t i = 0; i < d.totalRows; i++) {
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            d.flatSlices[i * NUM_STR_COLS + c].ptr = d.strCols[c][i].data();
            d.flatSlices[i * NUM_STR_COLS + c].len = d.strCols[c][i].size();
        }
    }
    // Debug: print len checksum to verify data matches Rust
    {
        uint64_t lenSum = 0;
        for (size_t i = 0; i < d.totalRows * NUM_STR_COLS; i++) lenSum += d.flatSlices[i].len;
        fprintf(stderr, "  flatSlices lenSum=%lu, first10:", (unsigned long)lenSum);
        for (size_t i = 0; i < 10 && i < d.flatSlices.size(); i++) fprintf(stderr, " %zu", d.flatSlices[i].len);
        fprintf(stderr, "\n");
    }
    return d;
}

// ─── Individual Steps (using real taper API) ────────────────────

// 1. Hash & position (creates real hashmap, uses BenchHash + BenchGetChunkPos)
__attribute__((noinline))
static uint64_t BenchHashAndPosition(const TestData& d) {
    // Create table once (static), avoid repeated mmap/munmap on large HT.
    static std::unique_ptr<taper::TaperFlatHashTable> cached;
    static size_t cachedChunks = 0;
    if (cachedChunks != d.numChunks) {
        cached = std::make_unique<taper::TaperFlatHashTable>(d.numChunks);
        cachedChunks = d.numChunks;
    }
    auto& table = *cached;
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

// 7. Serialize key (4 varchar) — pure serialize with arena allocator (no emplace overhead)
__attribute__((noinline))
static uint64_t BenchSerializeKey(const TestData& d) {
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes(NUM_STR_COLS, 0);
    std::vector<taper::ColumnKind> kinds(NUM_STR_COLS, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);

    uint64_t checksum = 0;
    for (size_t i = 0; i < d.totalRows; i++) {
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
        memcpy(row, &block, sizeof(block));
        taper::RowContainer::StoreValue<int64_t>(row, rc.AggStateOffset(), 0);
        checksum += reinterpret_cast<uint64_t>(row);
    }
    return checksum;
}

// 7a. Serialize with pre-allocated buffer (isolate pure serialize, no allocator cost)
__attribute__((noinline, aligned(64)))
static uint64_t BenchSerializePrealloc(const TestData& d) {
    // Pre-allocate one big buffer — no malloc during timing
    size_t maxPerRow = 4 * (1 + 4 + 30); // worst case: 4 cols × (1 + 4 + max_str_len ~30)
    size_t bufSize = d.totalRows * maxPerRow;
    uint8_t* buf = static_cast<uint8_t*>(malloc(bufSize));
    memset(buf, 0, bufSize); // touch all pages upfront (eliminate page faults)

    uint64_t checksum = 0;
    size_t numCols = NUM_STR_COLS;
    asm volatile("" : "+r"(numCols));
    uint8_t* wp = buf;
    for (size_t i = 0; i < d.totalRows; i++) {
        uint8_t* rowStart = wp;
        for (size_t c = 0; c < numCols; c++) {
            wp += taper::SerializeVarcharToBuffer(wp, d.slices[c][i].ptr, d.slices[c][i].len);
        }
        checksum += reinterpret_cast<uint64_t>(rowStart);
    }
    free(buf);
    return checksum;
}

// 7b. Just memcpy the data bytes (no header, no rowLenSize — pure memcpy cost)
__attribute__((noinline, aligned(64)))
static uint64_t BenchMemcpyOnly(const TestData& d) {
    size_t maxPerRow = 4 * 30;
    size_t bufSize = d.totalRows * maxPerRow;
    uint8_t* buf = static_cast<uint8_t*>(malloc(bufSize));
    memset(buf, 0, bufSize);

    uint64_t checksum = 0;
    size_t numCols = NUM_STR_COLS;
    asm volatile("" : "+r"(numCols));
    uint8_t* wp = buf;
    for (size_t i = 0; i < d.totalRows; i++) {
        for (size_t c = 0; c < numCols; c++) {
            memcpy(wp, d.slices[c][i].ptr, d.slices[c][i].len);
            wp += d.slices[c][i].len;
        }
        checksum += reinterpret_cast<uint64_t>(wp);
    }
    free(buf);
    return checksum;
}

// 7c. memcpy with flat array (eliminate vector-of-vector indirection)
__attribute__((noinline, aligned(64)))
static uint64_t BenchMemcpyFlat(const TestData& d) {
    size_t maxPerRow = 4 * 30;
    size_t bufSize = d.totalRows * maxPerRow;
    uint8_t* buf = static_cast<uint8_t*>(malloc(bufSize));
    memset(buf, 0, bufSize);

    uint64_t checksum = 0;
    size_t numCols = NUM_STR_COLS;
    asm volatile("" : "+r"(numCols));
    const taper::VarcharSlice* flat = d.flatSlices.data();
    uint8_t* wp = buf;
    for (size_t i = 0; i < d.totalRows; i++) {
        for (size_t c = 0; c < numCols; c++) {
            const auto& s = flat[i * NUM_STR_COLS + c];
            memcpy(wp, s.ptr, s.len);
            wp += s.len;
        }
        checksum += reinterpret_cast<uint64_t>(wp);
    }
    free(buf);
    return checksum;
}

// 9. Compare varchar (4 cols) — only times the compare loop, not the serialize/build
__attribute__((noinline))
static uint64_t BenchCompareVarchar(const TestData& d) {
    // ─── Setup (build groups once, not part of timing) ───
    // The bench harness calls this function `iters` times. We rebuild each time to be safe,
    // but only the compare loop at the bottom is the "hot" part.
    // In practice, the emplace overhead amortizes out with iters>1.
    taper::SimpleArenaAllocator pool;
    std::vector<size_t> keySizes(NUM_STR_COLS, 0);
    std::vector<taper::ColumnKind> kinds(NUM_STR_COLS, taper::ColumnKind::Varchar);
    taper::RowContainer rc(keySizes, kinds, 8, pool);
    taper::TaperFlatHashTable table(d.numChunks);
    int32_t colOffset = rc.ColumnAt(0).Offset();

    // Build groups via emplace (same as FULL pipeline step 2+3)
    std::vector<uint8_t*> groups(d.totalRows, nullptr);
    size_t numBatches = (d.totalRows + BATCH_SIZE - 1) / BATCH_SIZE;
    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, d.totalRows);
        int32_t batchLen = static_cast<int32_t>(end - start);
        const taper::VarcharSlice* colSlices[NUM_STR_COLS];
        for (size_t c = 0; c < NUM_STR_COLS; c++) colSlices[c] = d.slices[c].data() + start;

        table.EmplaceBatch(d.hashes.data() + start, batchLen,
            [](int32_t) { return false; },
            [&](uint32_t rowIdx, char* data) {
                char* row = rc.NewRow();
                size_t totalSize = 0;
                for (size_t c = 0; c < NUM_STR_COLS; c++)
                    totalSize += 1 + taper::ComputeRowLenSize(colSlices[c][rowIdx].len) + colSlices[c][rowIdx].len;
                uint8_t* block = pool.Allocate(static_cast<int64_t>(totalSize));
                uint8_t* wp = block;
                for (size_t c = 0; c < NUM_STR_COLS; c++)
                    wp += taper::SerializeVarcharToBuffer(wp, colSlices[c][rowIdx].ptr, colSlices[c][rowIdx].len);
                memcpy(row, &block, sizeof(block));
                uint64_t ptr = reinterpret_cast<uint64_t>(row);
                memcpy(data, &ptr, taper::ROW_PTR_SIZE);
                groups[start + rowIdx] = reinterpret_cast<uint8_t*>(row);
            },
            [&](uint32_t rowIdx, char* data, bool isNew) {
                if (!isNew) groups[start + rowIdx] = taper::GetRowPtr(data);
            }
        );
    }

    // ─── Timed: compare all rows (batch-driven, same as GetUnequalsNumWithDecode) ───
    uint64_t match_count = 0;
    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, d.totalRows);
        const taper::VarcharSlice* batchColSlices[NUM_STR_COLS];
        for (size_t c = 0; c < NUM_STR_COLS; c++) batchColSlices[c] = d.slices[c].data() + start;

        for (size_t ri = 0; ri < end - start; ri++) {
            size_t i = start + ri;
            if (!groups[i]) continue;
            const uint8_t* arenaPtr;
            memcpy(&arenaPtr, reinterpret_cast<const char*>(groups[i]) + colOffset, sizeof(arenaPtr));
            if (!arenaPtr) continue;
            const uint8_t* pos = arenaPtr;
            bool all_match = true;
            for (size_t c = 0; c < NUM_STR_COLS; c++) {
                if (!taper::CompareVarcharFromRow(pos, batchColSlices[c][ri].ptr, batchColSlices[c][ri].len)) {
                    all_match = false; break;
                }
                pos += taper::ComputeVarCharSerializedSize(pos);
            }
            if (all_match) match_count++;
        }
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
    size_t idx = 0;
    size_t numKeys = d.numKeys;
    for (size_t i = 0; i < d.totalRows; i++) {
        int64_t* p = reinterpret_cast<int64_t*>(rows[idx] + aggOffset);
        *p += d.values[i];
        checksum += static_cast<uint64_t>(*p);
        if (++idx >= numKeys) idx = 0;
    }
    return checksum;
}

// ─── Minimal isolated benchmarks (fixed data, no traversal) ─────────────
// These test single-function call overhead with zero data-access noise.

// M1. serialize_single: one fixed 12-byte string, 4M iterations (same as totalRows*4cols)
__attribute__((noinline))
static uint64_t BenchSerializeSingle(const TestData& d) {
    size_t iters = d.totalRows * NUM_STR_COLS;
    // Fixed 12-byte test string (typical key length "key_1234_c0")
    const uint8_t testStr[] = "key_12345_c0";
    size_t testLen = 12;
    // Pre-alloc buffer large enough
    size_t bufSize = iters * (1 + 1 + testLen);
    uint8_t* buf = static_cast<uint8_t*>(malloc(bufSize));
    memset(buf, 0, bufSize); // touch pages

    uint64_t checksum = 0;
    uint8_t* wp = buf;
    for (size_t i = 0; i < iters; i++) {
        wp += taper::SerializeVarcharToBuffer(wp, testStr, testLen);
    }
    checksum = reinterpret_cast<uint64_t>(wp);
    free(buf);
    return checksum;
}

// M2. memcpy_single: just memcpy 12 bytes, 4M iterations
__attribute__((noinline))
static uint64_t BenchMemcpySingle(const TestData& d) {
    size_t iters = d.totalRows * NUM_STR_COLS;
    const uint8_t testStr[] = "key_12345_c0";
    size_t testLen = 12;
    size_t bufSize = iters * testLen;
    uint8_t* buf = static_cast<uint8_t*>(malloc(bufSize));
    memset(buf, 0, bufSize);

    uint64_t checksum = 0;
    uint8_t* wp = buf;
    size_t len = testLen;
    asm volatile("" : "+r"(len)); // prevent constant propagation of len
    for (size_t i = 0; i < iters; i++) {
        memcpy(wp, testStr, len);
        wp += len;
    }
    checksum = reinterpret_cast<uint64_t>(wp);
    free(buf);
    return checksum;
}

// M3. compare_single: CompareVarcharFromRow on one pre-serialized 12-byte string, 4M iterations
__attribute__((noinline))
static uint64_t BenchCompareSingle(const TestData& d) {
    size_t iters = d.totalRows * NUM_STR_COLS;
    // Serialize one string to compare against
    uint8_t serialized[20];
    const uint8_t testStr[] = "key_12345_c0";
    size_t testLen = 12;
    taper::SerializeVarcharToBuffer(serialized, testStr, testLen);

    uint64_t match_count = 0;
    for (size_t i = 0; i < iters; i++) {
        if (taper::CompareVarcharFromRow(serialized, testStr, testLen))
            match_count++;
    }
    return match_count;
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
    if (shouldRun("7")) bench("7. serialize_key_4col", BenchSerializeKey);
    if (shouldRun("7")) bench("7a.serialize_prealloc", BenchSerializePrealloc);
    if (shouldRun("7")) bench("7b.memcpy_only", BenchMemcpyOnly);
    if (shouldRun("7")) bench("7c.memcpy_flat", BenchMemcpyFlat);
    if (shouldRun("9")) bench("9. compare_varchar_4col", BenchCompareVarchar);
    if (shouldRun("10")) bench("10. accumulate", BenchAccumulate);
    if (shouldRun("FULL") || shouldRun("11")) bench("FULL: pipeline", BenchFullPipeline);
    if (shouldRun("M")) bench("M1.serialize_single_12B", BenchSerializeSingle);
    if (shouldRun("M")) bench("M2.memcpy_single_12B", BenchMemcpySingle);
    if (shouldRun("M")) bench("M3.compare_single_12B", BenchCompareSingle);

    return 0;
}
