# Taper Micro Benchmarks

Isolated micro benchmarks proving that C++ and Rust Taper implementations
have identical per-function performance, but differ in combined pipeline
throughput due to compiler optimization differences on Kunpeng 920.

## Structure

```
taper-microbench/
├── cpp/                    # C++ implementations
│   ├── include/            # Shared headers (TaperHashMap, RowContainer, etc.)
│   ├── src/
│   │   ├── micro_hashmap_bench.cpp      # Pure hashmap insert+probe
│   │   ├── micro_functions_bench.cpp    # Individual function bench (6 cases)
│   │   ├── micro_cases_bench.cpp        # Isolated cases (serialize/compare/hashmap+row)
│   │   ├── micro_pipeline_bench.cpp     # Pipeline layers (A/C/C2/D)
│   │   └── profile_taper_standalone.cpp # Full workload runner (no framework)
│   └── CMakeLists.txt
├── rust/                   # Rust implementations (mirror of C++)
│   ├── src/
│   │   ├── bin/
│   │   │   ├── micro_hashmap_bench.rs
│   │   │   ├── micro_functions_bench.rs
│   │   │   ├── micro_cases_bench.rs
│   │   │   ├── micro_pipeline_bench.rs
│   │   │   └── profile_taper_standalone.rs
│   │   ├── lib.rs, taper_hashmap.rs, chunk.rs, bitmask.rs,
│   │   │   column_marshaller.rs, row_container.rs, batch_compare.rs
│   └── Cargo.toml
└── docs/
    └── (analysis reports)
```

## Benchmark Cases

### 1. `micro_functions_bench` — Individual Function Performance

Tests each function in isolation. **All are equally fast or C++ faster.**

| Function | What it tests |
|---|---|
| arena_alloc_only | `pool.Allocate(50)` × 1M |
| new_row_only | `rc.NewRow()` × 1M |
| serialize_4str_only | Serialize 4 varchars to arena × 1M rows |
| compare_4str_only | Compare 4 varchars (100% equal) × 1M rows |
| hashmap_probe_only | EmplaceBatch with trivial callback × 1M |
| setrowptr_only | 6-byte pointer encode/decode × 1M |

### 2. `micro_pipeline_bench` — Pipeline Layer Decomposition

Incrementally combines functions to show where the gap appears.

| Stage | What's included | Purpose |
|---|---|---|
| A | Hashmap probe only | Baseline |
| C | Hashmap + NewRow | + allocation |
| C2 | Hashmap + NewRow + Serialize | + varchar copy |
| D | Full pipeline (EmplaceTableWithDecode) | Complete |

**Key finding**: A and C are tied or C++ faster. C2 shows C++ suddenly 44% slower.

### 3. `micro_hashmap_bench` — Pure HashMap Performance

Inserts 100K unique keys + probes 1M (100% hit rate).
Tests TaperHashMap in isolation without any serialize/arena overhead.

### 4. `micro_cases_bench` — Isolated Semantic Cases

- `serialize_only`: All new rows, serialize 4 varchars, no hashmap
- `compare_only`: Pre-serialized rows, compare only, no hashmap
- `hashmap_plus_empty_row`: Hashmap + row allocation, no serialize

### 5. `profile_taper_standalone` — Full Workload Runner

Equivalent to the real benchmark but without any framework (no Google Benchmark / Criterion).
Supports `--profile-wait` for attaching perf to measure only the workload phase.

## Build & Run (Kunpeng 920)

```bash
# C++
CC=clang-22 CXX=clang++-22 cmake -B cpp/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=armv8-a+crc -flto=thin"
cmake --build cpp/build -j$(nproc)

# Rust
cd rust && cargo build --release && cd ..

# Run (bind to core for stability)
taskset -c 10 ./cpp/build/cpp_micro_functions
taskset -c 10 ./rust/target/release/micro_functions_bench

taskset -c 10 ./cpp/build/cpp_micro_pipeline 0.1 C2 100
taskset -c 10 ./rust/target/release/micro_pipeline_bench 0.1 C2 100
```

## Key Results (Kunpeng 920, clang-22 vs rustc/LLVM-22)

### Individual Functions — C++ ≥ Rust

| Function | C++ (ms) | Rust (ms) |
|---|---|---|
| arena_alloc | 0.6 | 2.9 |
| new_row | 10.9 | 14.0 |
| serialize_4str | 41.6 | 44.7 |
| compare_4str | 1.8 | 19.8 |
| hashmap_probe | 47.8 | 52.3 |

### Combined Pipeline — Rust wins by 44%

| Stage | C++ (ms) | Rust (ms) |
|---|---|---|
| C2 (hashmap+newrow+serialize) | 150.4 | 84.2 |

### Root Cause Evidence

- `perf report`: C++ lambda is a separate symbol (79.63%), Rust closure inlined (29.20% in emplace_batch_full)
- `perf stat`: C++ IPC=1.59, Rust IPC=2.38 (same L1 cache miss rate)
- `__attribute__((flatten))`: 126ms (19% improvement, still 50% slower than Rust)
- Full LTO: no improvement (clang cost model refuses to inline large lambda body)

### Conclusion

The performance gap exists only when functions are combined in the EmplaceBatch callback loop.
Rust's monomorphization guarantees closure inlining; clang-22 does not inline the lambda.
This is a compiler backend optimization difference, not an algorithm or data structure issue.
