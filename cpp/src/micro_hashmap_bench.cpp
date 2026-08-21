/// Minimal TaperHashMap microbench — pure insert + probe, no varchar/arena/RowContainer.
/// Tests whether C++ TaperFlatHashTable is inherently slower than Rust TaperHashMap.
///
/// Usage: ./cpp_micro_hashmap [num_keys] [num_probe] [iterations]
/// Default: 100000 keys, 1000000 probes, 10 iterations

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <random>

#include "taper_hashtable.h"

static constexpr size_t DEFAULT_NUM_KEYS = 100000;
static constexpr size_t DEFAULT_NUM_PROBE = 1000000;
static constexpr size_t DEFAULT_ITERS = 10;

using Clock = std::chrono::high_resolution_clock;
static double elapsed_ms(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

__attribute__((noinline))
static uint64_t RunInsertProbe(const int64_t* hashes, size_t numRows, size_t numChunks) {
    taper::TaperFlatHashTable table(numChunks);
    uint64_t sum = 0;

    // Use EmplaceBatch with trivial init/update — just count
    table.EmplaceBatch(hashes, static_cast<int32_t>(numRows),
        [](int32_t) { return false; },
        [](uint32_t /*rowIdx*/, char* data) {
            // on_init: write a dummy value (simulates setting row pointer)
            memset(data, 0x42, taper::ROW_PTR_SIZE);
        },
        [&sum](uint32_t /*rowIdx*/, char* data, bool isNew) {
            // on_update: accumulate (simulates agg += value)
            if (!isNew) {
                sum += 1;
            }
        }
    );

    return table.Size() + sum;
}

int main(int argc, char** argv) {
    size_t numKeys = DEFAULT_NUM_KEYS;
    size_t numProbe = DEFAULT_NUM_PROBE;
    size_t numIters = DEFAULT_ITERS;

    if (argc > 1) numKeys = static_cast<size_t>(atoi(argv[1]));
    if (argc > 2) numProbe = static_cast<size_t>(atoi(argv[2]));
    if (argc > 3) numIters = static_cast<size_t>(atoi(argv[3]));

    size_t totalRows = numKeys + numProbe;
    size_t numChunks = 1;
    while (numChunks * 8 < static_cast<size_t>(numKeys / 0.85)) numChunks *= 2;
    // Ensure enough for all distinct keys (keys + some misses from probe)
    while (numChunks * 8 < totalRows) numChunks *= 2;

    fprintf(stderr, "=== C++ Micro HashMap Bench ===\n");
    fprintf(stderr, "numKeys=%zu, numProbe=%zu, totalRows=%zu\n", numKeys, numProbe, totalRows);
    fprintf(stderr, "numChunks=%zu, capacity=%zu\n", numChunks, numChunks * 8);
    fprintf(stderr, "Iterations: %zu (+ 1 warmup)\n\n", numIters);

    // Generate hashes: first numKeys are unique, then numProbe repeat from those
    fprintf(stderr, "Generating hashes...\n");
    std::mt19937_64 rng(42);
    std::vector<int64_t> allHashes(totalRows);
    // Build: unique hashes
    for (size_t i = 0; i < numKeys; i++) {
        allHashes[i] = static_cast<int64_t>(rng());
    }
    // Probe: randomly pick from build hashes (100% hit rate for pure probe test)
    for (size_t i = numKeys; i < totalRows; i++) {
        allHashes[i] = allHashes[rng() % numKeys];
    }
    fprintf(stderr, "Done.\n\n");

    // Warmup
    fprintf(stderr, "Warmup...\n");
    volatile uint64_t w = RunInsertProbe(allHashes.data(), totalRows, numChunks);
    (void)w;
    fprintf(stderr, "Warmup done.\n\n");

    // Timed
    fprintf(stderr, "Running %zu iterations...\n", numIters);
    auto t0 = Clock::now();

    volatile uint64_t checksum = 0;
    for (size_t iter = 0; iter < numIters; iter++) {
        checksum = RunInsertProbe(allHashes.data(), totalRows, numChunks);
    }

    auto t1 = Clock::now();
    double total_ms = elapsed_ms(t0, t1);
    double per_iter_ms = total_ms / numIters;
    double items_per_sec = totalRows / (per_iter_ms / 1000.0);

    printf("=== Results ===\n");
    printf("Total:         %.3f ms\n", total_ms);
    printf("Per iter:      %.3f ms\n", per_iter_ms);
    printf("Items/sec:     %.3f M/s\n", items_per_sec / 1e6);
    printf("Checksum:      %lu\n", (unsigned long)checksum);

    return 0;
}
