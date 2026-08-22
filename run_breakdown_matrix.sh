#!/bin/bash
# Benchmark matrix: HT_SIZE × Selectivity
# Usage: ./run_breakdown_matrix.sh [iters]
# Output: side-by-side C++ vs Rust for each (ht, sel) combo

ITERS=${1:-5}
CPP_BIN="./cpp/build/cpp_emplace_breakdown"
RUST_BIN="./rust/target/release/emplace_breakdown_bench"

HT_SIZES=(16384 65536 262144 1048576)
SELECTIVITIES=(0.1 0.3 0.5 0.7 0.9)

for ht in "${HT_SIZES[@]}"; do
  for sel in "${SELECTIVITIES[@]}"; do
    echo "================================================================"
    echo "HT=$ht  SEL=$sel"
    echo "----------------------------------------------------------------"
    echo "[C++]"
    taskset -c 0 "$CPP_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | grep "ms"
    echo ""
    echo "[Rust]"
    taskset -c 0 "$RUST_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | grep "ms"
    echo ""
  done
done
