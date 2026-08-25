#!/bin/bash
# Benchmark matrix: HT_SIZE × Selectivity (horizontal TSV)
# Only extracts main steps (1,3,4,5,7,9,10,FULL)
# Usage: ./run_breakdown_matrix.sh [iters]

ITERS=${1:-5}
CPP_BIN="./cpp/build/cpp_emplace_breakdown"
RUST_BIN="./rust/target/release/emplace_breakdown_bench"

HT_SIZES=(16384 65536 262144 1048576)
SELECTIVITIES=(0.1 0.3 0.5 0.7 0.9)

# Header
printf "HT\tSel\tLang\thash_pos\tload_tags\tmatch_tag\tcompare_key\tserialize\tcompare_vc\taccumulate\tFULL\n"

for ht in "${HT_SIZES[@]}"; do
  for sel in "${SELECTIVITIES[@]}"; do
    # C++ — extract only lines starting with "1." "3." "4." "5." "6." "7." "8." "9." "10." "FULL"
    cpp_times=$(taskset -c 0 "$CPP_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | \
      grep -E "^(1\.|3\.|4\.|5\.|7\.|9\.|10\.|FULL)" | \
      awk '{for(i=1;i<=NF;i++){if($i=="ms"){print $(i-1)}}}' | tr '\n' '\t' | sed 's/\t$//')
    printf "%s\t%s\tC++\t%s\n" "$ht" "$sel" "$cpp_times"

    # Rust
    rust_times=$(taskset -c 0 "$RUST_BIN" "$sel" "$ITERS" "$ht" 2>/dev/null | \
      grep -E "^(1\.|3\.|4\.|5\.|7\.|9\.|10\.|FULL)" | \
      awk '{for(i=1;i<=NF;i++){if($i=="ms"){print $(i-1)}}}' | tr '\n' '\t' | sed 's/\t$//')
    printf "%s\t%s\tRust\t%s\n" "$ht" "$sel" "$rust_times"
  done
done
