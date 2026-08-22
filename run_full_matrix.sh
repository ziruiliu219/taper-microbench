#!/bin/bash
# Benchmark matrix: HT_SIZE × Selectivity (step-by-step bench)
# Usage: ./run_full_matrix.sh [iters]
# Output: TSV to stdout (redirect to file for analysis)

ITERS=${1:-5}
CPP_BIN="./cpp/build/cpp_step_by_step"
RUST_BIN="./rust/target/release/step_by_step_bench"

HT_SIZES=(16384 65536 262144 1048576)
SELECTIVITIES=(0.1 0.3 0.5 0.7 0.9)

echo -e "HT\tSel\tLang\tprecompute\tnew_row\tserialize\tstore_val\tcompare\taccumulate\tFULL"

for ht in "${HT_SIZES[@]}"; do
  for sel in "${SELECTIVITIES[@]}"; do
    # C++
    cpp_vals=$(taskset -c 10 "$CPP_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | \
      grep "ms" | awk '{for(i=1;i<=NF;i++){if($(i+1)=="ms"){printf "%s\t",$i}}}' | sed 's/\t$//')
    echo -e "${ht}\t${sel}\tC++\t${cpp_vals}"

    # Rust
    rust_vals=$(taskset -c 10 "$RUST_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | \
      grep "ms" | awk '{for(i=1;i<=NF;i++){if($(i+1)=="ms"){printf "%s\t",$i}}}' | sed 's/\t$//')
    echo -e "${ht}\t${sel}\tRust\t${rust_vals}"
  done
done
