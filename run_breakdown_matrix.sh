#!/bin/bash
# Full benchmark matrix using emplace_breakdown_bench
# Usage: ./run_breakdown_matrix.sh [iters]
# Output: TSV to stdout

ITERS=${1:-5}
CPP_BIN="./cpp/build/cpp_emplace_breakdown"
RUST_BIN="./rust/target/release/emplace_breakdown_bench"

HT_SIZES=(16384 65536 262144 1048576)
LOAD_FACTORS=(0.5 0.75)
SELECTIVITIES=(0.1 0.3 0.5 0.7 0.9)

echo -e "HT\tLF\tSel\tLang\thash_pos\tprefetch\tload_tags\tmatch_tag\tcompare_key\tnew_row\tserialize\tstore_val\tcompare_vc\taccumulate\tFULL"

for ht in "${HT_SIZES[@]}"; do
  for lf in "${LOAD_FACTORS[@]}"; do
    for sel in "${SELECTIVITIES[@]}"; do
      # C++
      cpp_vals=$(taskset -c 10 "$CPP_BIN" "$sel" "$ITERS" "$ht" "$lf" 2>/dev/null | \
        grep "ms" | awk '{for(i=1;i<=NF;i++){if($(i+1)=="ms"){printf "%s\t",$i}}}' | sed 's/\t$//')
      echo -e "${ht}\t${lf}\t${sel}\tC++\t${cpp_vals}"

      # Rust
      rust_vals=$(taskset -c 10 "$RUST_BIN" "$sel" "$ITERS" "$ht" "$lf" 2>/dev/null | \
        grep "ms" | awk '{for(i=1;i<=NF;i++){if($(i+1)=="ms"){printf "%s\t",$i}}}' | sed 's/\t$//')
      echo -e "${ht}\t${lf}\t${sel}\tRust\t${rust_vals}"
    done
  done
done
