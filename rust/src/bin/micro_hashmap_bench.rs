//! Minimal TaperHashMap microbench — pure insert + probe, no varchar/arena/RowContainer.
//! Tests whether Rust TaperHashMap is inherently faster than C++ TaperFlatHashTable.
//!
//! Usage: ./micro_hashmap_bench [num_keys] [num_probe] [iterations]
//! Default: 100000 keys, 1000000 probes, 10 iterations

use std::time::Instant;
use taper_hashmap::taper_hashmap::TaperHashMap;
use taper_hashmap::chunk::SlotValue;

const DEFAULT_NUM_KEYS: usize = 100000;
const DEFAULT_NUM_PROBE: usize = 1000000;
const DEFAULT_ITERS: usize = 10;

#[inline(never)]
fn run_insert_probe(hashes: &[u64], num_chunks: usize) -> u64 {
    let mut table = TaperHashMap::with_capacity(num_chunks);
    let mut sum: u64 = 0;

    // Use emplace_batch_full with trivial init/update — just count
    table.emplace_batch_full(
        hashes,
        &|_: usize, _: &SlotValue| -> bool { true }, // key_cmp: always match on hash (no real key)
        &mut |_i: usize, sv: &mut SlotValue| {
            // on_init: write dummy value
            sv.bytes = [0x42; 6];
        },
        &mut |_i: usize, _sv: &SlotValue, is_new: bool| {
            // on_update: count hits
            if !is_new {
                sum += 1;
            }
        },
    );

    table.len() as u64 + sum
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let num_keys = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_NUM_KEYS);
    let num_probe = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_NUM_PROBE);
    let num_iters = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_ITERS);

    let total_rows = num_keys + num_probe;
    let mut num_chunks = 1usize;
    while num_chunks * 8 < (num_keys as f64 / 0.85) as usize { num_chunks *= 2; }
    while num_chunks * 8 < total_rows { num_chunks *= 2; }

    eprintln!("=== Rust Micro HashMap Bench ===");
    eprintln!("numKeys={}, numProbe={}, totalRows={}", num_keys, num_probe, total_rows);
    eprintln!("numChunks={}, capacity={}", num_chunks, num_chunks * 8);
    eprintln!("Iterations: {} (+ 1 warmup)\n", num_iters);

    // Generate hashes
    eprintln!("Generating hashes...");
    use rand_mt::Mt19937GenRand64;
    let mut rng = Mt19937GenRand64::new(42);
    let mut all_hashes: Vec<u64> = Vec::with_capacity(total_rows);
    for _ in 0..num_keys {
        all_hashes.push(rng.next_u64());
    }
    for _ in num_keys..total_rows {
        let idx = (rng.next_u64() as usize) % num_keys;
        all_hashes.push(all_hashes[idx]);
    }
    eprintln!("Done.\n");

    // Warmup
    eprintln!("Warmup...");
    let _w = run_insert_probe(&all_hashes, num_chunks);
    eprintln!("Warmup done.\n");

    // Timed
    eprintln!("Running {} iterations...", num_iters);
    let t0 = Instant::now();

    let mut checksum: u64 = 0;
    for _ in 0..num_iters {
        checksum = run_insert_probe(&all_hashes, num_chunks);
    }

    let elapsed = t0.elapsed();
    let total_ms = elapsed.as_secs_f64() * 1000.0;
    let per_iter_ms = total_ms / num_iters as f64;
    let items_per_sec = total_rows as f64 / (per_iter_ms / 1000.0);

    println!("=== Results ===");
    println!("Total:         {:.3} ms", total_ms);
    println!("Per iter:      {:.3} ms", per_iter_ms);
    println!("Items/sec:     {:.3} M/s", items_per_sec / 1e6);
    println!("Checksum:      {}", checksum);
}
