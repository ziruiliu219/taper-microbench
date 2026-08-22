#!/bin/bash
# Benchmark matrix: HT_SIZE × Selectivity (horizontal TSV format)
# Usage: ./run_breakdown_matrix.sh [iters]
# Output: TSV with one row per (ht, sel, lang), columns = step times

ITERS=${1:-5}
CPP_BIN="./cpp/build/cpp_emplace_breakdown"
RUST_BIN="./rust/target/release/emplace_breakdown_bench"

HT_SIZES=(16384 65536 262144 1048576)
SELECTIVITIES=(0.1 0.3 0.5 0.7 0.9)

# Extract ms values from benchmark output (all lines containing "ms")
extract_times() {
  grep "ms" | awk '{for(i=1;i<=NF;i++){if($i=="ms"){printf "%s\t",$(i-1)}}}' | sed 's/\t$//'
}

# Print header: step names from a single run
header=$(taskset -c 0 "$CPP_BIN" "0.5" "1" "16384" 2>/dev/null | grep "ms" | awk '{print $1}' | tr '\n' '\t' | sed 's/\t$//')
printf "HT\tSel\tLang\t%s\n" "$header"

for ht in "${HT_SIZES[@]}"; do
  for sel in "${SELECTIVITIES[@]}"; do
    # C++
    cpp_times=$(taskset -c 0 "$CPP_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | extract_times)
    printf "%s\t%s\tC++\t%s\n" "$ht" "$sel" "$cpp_times"

    # Rust
    rust_times=$(taskset -c 0 "$RUST_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | extract_times)
    printf "%s\t%s\tRust\t%s\n" "$ht" "$sel" "$rust_times"
  done
done
