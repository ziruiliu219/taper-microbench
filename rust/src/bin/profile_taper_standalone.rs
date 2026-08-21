//! Standalone profile runner for Taper Rust — no Criterion dependency.
//! Identical workload to the C++ profile runner.
//!
//! Usage:
//!   ./profile_taper_standalone <sel> [iterations] [--profile-wait]
//!
//! Examples:
//!   ./profile_taper_standalone 0.1           # sel=0.1, 10 iterations
//!   ./profile_taper_standalone 0.9 100       # sel=0.9, 100 iterations
//!   ./profile_taper_standalone 0.1 50 --profile-wait

use std::io::{self, BufRead};
use std::time::Instant;
use taper_hashmap::column_marshaller::{TaperColumnSerializeHandler, ColumnDesc, ColumnInput};
use xxhash_rust::xxh3::xxh3_64_with_seed;

// ═══════════════════════════════════════════════════════════════════
// Parameters (matching C++ exactly)
// ═══════════════════════════════════════════════════════════════════

const NUM_STR_COLS: usize = 4;
const NUM_INT_COLS: usize = 0;
const HT_SIZE: usize = 16384;
const LOAD_FACTOR: f64 = 0.50;
const NUM_PROBE_ROWS: usize = 1_000_000;
const BATCH_SIZE: usize = 410;
const SEED: u64 = 42;

// ═══════════════════════════════════════════════════════════════════
// Data generation (matching C++ GenData exactly)
// ═══════════════════════════════════════════════════════════════════

use rand_mt::Mt19937GenRand64;

#[inline]
fn hash_bytes(data: &[u8], seed: u64) -> u64 {
    xxh3_64_with_seed(data, seed)
}

fn gen_string(base: &str, id: usize, col: usize) -> Vec<u8> {
    format!("{}_{}_c{}", base, id, col).into_bytes()
}

struct BenchData {
    str_cols: Vec<Vec<Vec<u8>>>,
    #[allow(dead_code)]
    int_cols: Vec<Vec<i64>>,
    hashes: Vec<u64>,
    values: Vec<i64>,
    total_rows: usize,
}

fn gen_data(num_keys: usize, sel: f64) -> BenchData {
    let mut rng = Mt19937GenRand64::new(SEED);

    let mut str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| (0..num_keys).map(|i| gen_string("key", i, c)).collect())
        .collect();

    let build_hashes: Vec<u64> = (0..num_keys)
        .map(|i| {
            let mut h = 0u64;
            for c in 0..NUM_STR_COLS {
                h = hash_bytes(&str_cols[c][i], h);
            }
            h
        })
        .collect();

    let build_values: Vec<i64> = (0..num_keys).map(|i| (i % 1000) as i64).collect();

    let num_hits = (NUM_PROBE_ROWS as f64 * sel) as usize;
    let num_misses = NUM_PROBE_ROWS - num_hits;

    let mut probe_str_cols: Vec<Vec<Vec<u8>>> = vec![Vec::with_capacity(NUM_PROBE_ROWS); NUM_STR_COLS];
    let mut probe_hashes: Vec<u64> = Vec::with_capacity(NUM_PROBE_ROWS);

    for _ in 0..num_hits {
        let idx = (rng.next_u64() as usize) % num_keys;
        for c in 0..NUM_STR_COLS {
            probe_str_cols[c].push(str_cols[c][idx].clone());
        }
        probe_hashes.push(build_hashes[idx]);
    }

    for i in 0..num_misses {
        let mut h = 0u64;
        for c in 0..NUM_STR_COLS {
            let s = format!("miss_{}_{}", i, c).into_bytes();
            h = hash_bytes(&s, h);
            probe_str_cols[c].push(s);
        }
        probe_hashes.push(h);
    }

    // Shuffle
    let mut order: Vec<usize> = (0..NUM_PROBE_ROWS).collect();
    for i in (1..NUM_PROBE_ROWS).rev() {
        order.swap(i, (rng.next_u64() as usize) % (i + 1));
    }
    let probe_str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| order.iter().map(|&i| probe_str_cols[c][i].clone()).collect())
        .collect();
    let probe_hashes: Vec<u64> = order.iter().map(|&i| probe_hashes[i]).collect();
    let probe_values: Vec<i64> = (0..NUM_PROBE_ROWS).map(|i| (i % 1000) as i64).collect();

    // Combine
    let total_rows = num_keys + NUM_PROBE_ROWS;
    for c in 0..NUM_STR_COLS {
        str_cols[c].extend(probe_str_cols[c].iter().cloned());
    }
    let mut all_hashes = build_hashes;
    all_hashes.extend_from_slice(&probe_hashes);
    let mut all_values = build_values;
    all_values.extend_from_slice(&probe_values);

    BenchData {
        str_cols,
        int_cols: Vec::new(),
        hashes: all_hashes,
        values: all_values,
        total_rows,
    }
}

// ═══════════════════════════════════════════════════════════════════
// Core workload (noinline for perf visibility)
// ═══════════════════════════════════════════════════════════════════

#[inline(never)]
fn run_workload(data: &BenchData, num_chunks: usize) -> usize {
    let mut col_descs: Vec<ColumnDesc> = Vec::new();
    for _ in 0..NUM_STR_COLS { col_descs.push(ColumnDesc::Varchar); }
    for _ in 0..NUM_INT_COLS { col_descs.push(ColumnDesc::Int64); }

    let mut table = TaperColumnSerializeHandler::new(&col_descs, 8, num_chunks);
    let total_rows = data.hashes.len();
    let num_batches = (total_rows + BATCH_SIZE - 1) / BATCH_SIZE;

    for batch_idx in 0..num_batches {
        let start = batch_idx * BATCH_SIZE;
        let end = (start + BATCH_SIZE).min(total_rows);

        let batch_hashes = &data.hashes[start..end];
        let batch_values = &data.values[start..end];
        let str_slices: Vec<Vec<&[u8]>> = (0..NUM_STR_COLS)
            .map(|c| data.str_cols[c][start..end].iter().map(|s| s.as_slice()).collect())
            .collect();

        let mut columns: Vec<ColumnInput> = Vec::new();
        for c in 0..NUM_STR_COLS { columns.push(ColumnInput::Varchar(&str_slices[c])); }
        for c in 0..NUM_INT_COLS { columns.push(ColumnInput::Int64(&data.int_cols[c][start..end])); }

        table.emplace_table_with_decode(batch_hashes, &columns, batch_values);
    }

    table.num_groups()
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

fn main() {
    let args: Vec<String> = std::env::args().collect();

    let mut sel: f64 = 0.1;
    let mut num_iters: usize = 10;
    let mut profile_wait = false;

    let mut positional = 0;
    for arg in args.iter().skip(1) {
        if arg == "--profile-wait" {
            profile_wait = true;
        } else {
            match positional {
                0 => sel = arg.parse().unwrap_or(0.1),
                1 => num_iters = arg.parse().unwrap_or(10),
                _ => {}
            }
            positional += 1;
        }
    }

    if sel <= 0.0 || sel > 1.0 {
        eprintln!("sel must be in (0,1]");
        std::process::exit(1);
    }
    if num_iters == 0 {
        eprintln!("iterations must be > 0");
        std::process::exit(1);
    }

    let num_keys = (HT_SIZE as f64 * LOAD_FACTOR) as usize;
    let num_misses = NUM_PROBE_ROWS - (NUM_PROBE_ROWS as f64 * sel) as usize;
    let distinct_keys = num_keys + num_misses;
    let min_slots = ((distinct_keys as f64 / 0.85) as usize).max(8);
    let num_chunks = ((min_slots + 7) / 8).next_power_of_two();

    eprintln!("=== Rust Profile Runner ===");
    eprintln!("Config: 4str_0int, ht={}, lf={:.2}, sel={:.1}", HT_SIZE, LOAD_FACTOR, sel);
    eprintln!("numKeys={}, numProbe={}, totalRows={}", num_keys, NUM_PROBE_ROWS, num_keys + NUM_PROBE_ROWS);
    eprintln!("distinctKeys={}, numChunks={}, capacity={}", distinct_keys, num_chunks, num_chunks * 8);
    eprintln!("Iterations: {} (+ 1 warmup)", num_iters);

    // Data generation (timed separately)
    eprintln!("Generating data...");
    let t_gen0 = Instant::now();
    let data = gen_data(num_keys, sel);
    let t_gen1 = Instant::now();
    let gen_ms = t_gen1.duration_since(t_gen0).as_secs_f64() * 1000.0;
    eprintln!("Data generated. totalRows={} ({:.1} ms)\n", data.total_rows, gen_ms);

    // Warmup (timed separately)
    eprintln!("Warmup...");
    let t_warm0 = Instant::now();
    let warmup_groups = run_workload(&data, num_chunks);
    let t_warm1 = Instant::now();
    let warm_ms = t_warm1.duration_since(t_warm0).as_secs_f64() * 1000.0;
    eprintln!("Warmup done (groups={}, {:.1} ms)\n", warmup_groups, warm_ms);

    // Profile-wait mode
    if profile_wait {
        eprintln!("READY pid={}", std::process::id());
        eprintln!("Attach perf then press Enter to start measured iterations...");
        let stdin = io::stdin();
        let _ = stdin.lock().lines().next();
    }

    // Timed iterations
    eprintln!("Running {} iterations...", num_iters);
    let t0 = Instant::now();

    let mut groups = 0usize;
    for _ in 0..num_iters {
        groups = run_workload(&data, num_chunks);
    }

    let t1 = Instant::now();
    let total_ms = t1.duration_since(t0).as_secs_f64() * 1000.0;
    let per_iter_ms = total_ms / num_iters as f64;
    let per_iter_ns = per_iter_ms * 1e6;
    let items_per_sec = data.total_rows as f64 / (per_iter_ns / 1e9);

    println!("=== Results ===");
    println!("Setup time:        {:.3} ms", gen_ms);
    println!("Warmup time:       {:.3} ms", warm_ms);
    println!("Workload total:    {:.3} ms", total_ms);
    println!("Per iteration:     {:.3} ms", per_iter_ms);
    println!("ns/iteration:      {:.0}", per_iter_ns);
    println!("Items/sec:         {:.3} M/s", items_per_sec / 1e6);
    println!("Groups:            {}", groups);
    println!("Checksum (groups): {}", groups);
}
