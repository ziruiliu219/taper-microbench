//! Real batch pipeline micro benchmark — incrementally adds layers.
//!
//! Case A: hashmap probe only
//! Case C: hashmap + NewRow (no varchar serialize)
//! Case D: full pipeline (EmplaceTableWithDecode)
//!
//! Usage: ./micro_pipeline_bench <sel> [iterations]

use std::time::Instant;
use taper_hashmap::column_marshaller::{TaperColumnSerializeHandler, ColumnDesc, ColumnInput};
use taper_hashmap::taper_hashmap::TaperHashMap;
use taper_hashmap::chunk::SlotValue;
use taper_hashmap::row_container::{RowContainer, ColumnKind};
use xxhash_rust::xxh3::xxh3_64_with_seed;
use rand_mt::Mt19937GenRand64;

const NUM_STR_COLS: usize = 4;
const HT_SIZE: usize = 16384;
const LOAD_FACTOR: f64 = 0.50;
const NUM_PROBE_ROWS: usize = 1_000_000;
const BATCH_SIZE: usize = 410;
const SEED: u64 = 42;

fn hash_bytes(data: &[u8], seed: u64) -> u64 { xxh3_64_with_seed(data, seed) }
fn gen_string(base: &str, id: usize, col: usize) -> Vec<u8> { format!("{}_{}_c{}", base, id, col).into_bytes() }

struct BenchData {
    str_cols: Vec<Vec<Vec<u8>>>,
    hashes: Vec<u64>,
    values: Vec<i64>,
    total_rows: usize,
}

fn gen_data(num_keys: usize, sel: f64) -> BenchData {
    let mut rng = Mt19937GenRand64::new(SEED);
    let mut str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| (0..num_keys).map(|i| gen_string("key", i, c)).collect()).collect();
    let build_hashes: Vec<u64> = (0..num_keys).map(|i| {
        let mut h = 0u64;
        for c in 0..NUM_STR_COLS { h = hash_bytes(&str_cols[c][i], h); }
        h
    }).collect();
    let build_values: Vec<i64> = (0..num_keys).map(|i| (i % 1000) as i64).collect();

    let num_hits = (NUM_PROBE_ROWS as f64 * sel) as usize;
    let num_misses = NUM_PROBE_ROWS - num_hits;
    let mut probe_str_cols: Vec<Vec<Vec<u8>>> = vec![Vec::with_capacity(NUM_PROBE_ROWS); NUM_STR_COLS];
    let mut probe_hashes: Vec<u64> = Vec::with_capacity(NUM_PROBE_ROWS);
    for _ in 0..num_hits { let idx = (rng.next_u64() as usize) % num_keys; for c in 0..NUM_STR_COLS { probe_str_cols[c].push(str_cols[c][idx].clone()); } probe_hashes.push(build_hashes[idx]); }
    for i in 0..num_misses { let mut h = 0u64; for c in 0..NUM_STR_COLS { let s = format!("miss_{}_{}", i, c).into_bytes(); h = hash_bytes(&s, h); probe_str_cols[c].push(s); } probe_hashes.push(h); }

    let mut order: Vec<usize> = (0..NUM_PROBE_ROWS).collect();
    for i in (1..NUM_PROBE_ROWS).rev() { order.swap(i, (rng.next_u64() as usize) % (i + 1)); }
    let probe_str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS).map(|c| order.iter().map(|&i| probe_str_cols[c][i].clone()).collect()).collect();
    let probe_hashes: Vec<u64> = order.iter().map(|&i| probe_hashes[i]).collect();
    let probe_values: Vec<i64> = (0..NUM_PROBE_ROWS).map(|i| (i % 1000) as i64).collect();

    let total_rows = num_keys + NUM_PROBE_ROWS;
    for c in 0..NUM_STR_COLS { str_cols[c].extend(probe_str_cols[c].iter().cloned()); }
    let mut all_hashes = build_hashes; all_hashes.extend_from_slice(&probe_hashes);
    let mut all_values = build_values; all_values.extend_from_slice(&probe_values);

    BenchData { str_cols, hashes: all_hashes, values: all_values, total_rows }
}

// ─── Case A: hashmap only ────────────────────────────────────────────────
#[inline(never)]
fn run_hashmap_only(data: &BenchData, num_chunks: usize) -> usize {
    let mut table = TaperHashMap::with_capacity(num_chunks);
    let mut sum: u64 = 0;
    table.emplace_batch_full(
        &data.hashes,
        &|_: usize, _: &SlotValue| -> bool { true },
        &mut |_i: usize, sv: &mut SlotValue| { sv.bytes = [0x42; 6]; },
        &mut |_i: usize, _sv: &SlotValue, is_new: bool| { if !is_new { sum += 1; } },
    );
    table.len() + sum as usize
}

// ─── Case C: hashmap + NewRow ────────────────────────────────────────────
#[inline(never)]
fn run_hashmap_plus_newrow(data: &BenchData, num_chunks: usize) -> usize {
    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);
    let mut table = TaperHashMap::with_capacity(num_chunks);
    let agg_offset = rc.agg_state_offset();
    let mut sum: u64 = 0;

    let rc_ptr = &mut rc as *mut RowContainer;
    table.emplace_batch_full(
        &data.hashes,
        &|_: usize, _: &SlotValue| -> bool { true },
        &mut |i: usize, sv: &mut SlotValue| {
            let rc = unsafe { &mut *rc_ptr };
            let row = rc.new_row();
            RowContainer::store_value::<i64>(row, agg_offset, data.values[i]);
            sv.set_ptr(row as *const u8);
        },
        &mut |_i: usize, _sv: &SlotValue, is_new: bool| { if !is_new { sum += 1; } },
    );
    table.len() + sum as usize
}

// ─── Case C2: hashmap + NewRow + serialize (no batch compare) ─────────────
#[inline(never)]
fn run_hashmap_plus_serialize(data: &BenchData, num_chunks: usize) -> usize {
    use taper_hashmap::column_marshaller::{serialize_varchar_to_buffer, compute_row_len_size};

    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);
    let mut table = TaperHashMap::with_capacity(num_chunks);
    let agg_offset = rc.agg_state_offset();
    let mut sum: u64 = 0;

    let total_rows = data.hashes.len();
    let num_batches = (total_rows + BATCH_SIZE - 1) / BATCH_SIZE;

    for batch_idx in 0..num_batches {
        let start = batch_idx * BATCH_SIZE;
        let end = (start + BATCH_SIZE).min(total_rows);
        let batch_hashes = &data.hashes[start..end];

        let str_slices: Vec<Vec<&[u8]>> = (0..NUM_STR_COLS)
            .map(|c| data.str_cols[c][start..end].iter().map(|s| s.as_slice()).collect())
            .collect();

        let rc_ptr = &mut rc as *mut RowContainer;

        table.emplace_batch_full(
            batch_hashes,
            &|_: usize, _: &SlotValue| -> bool { true },
            &mut |i: usize, sv: &mut SlotValue| {
                let rc = unsafe { &mut *rc_ptr };
                let row = rc.new_row();
                // Serialize 4 varchars into merged block
                let mut total_size = 0usize;
                for c in 0..NUM_STR_COLS {
                    let s = str_slices[c][i];
                    total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
                }
                let block = rc.arena_alloc(total_size);
                let mut wp = block;
                for c in 0..NUM_STR_COLS {
                    let s = str_slices[c][i];
                    let written = unsafe { serialize_varchar_to_buffer(wp, s) };
                    wp = unsafe { wp.add(written) };
                }
                // Store pointer at offset 0
                unsafe { (row as *mut *const u8).write_unaligned(block as *const u8); }
                RowContainer::store_value::<i64>(row, agg_offset, data.values[start + i]);
                sv.set_ptr(row as *const u8);
            },
            &mut |_i: usize, _sv: &SlotValue, is_new: bool| { if !is_new { sum += 1; } },
        );
    }
    table.len() + sum as usize
}

// ─── Case D: full pipeline ───────────────────────────────────────────────
#[inline(never)]
fn run_full_pipeline(data: &BenchData, num_chunks: usize) -> usize {
    let mut col_descs: Vec<ColumnDesc> = vec![ColumnDesc::Varchar; NUM_STR_COLS];
    let mut table = TaperColumnSerializeHandler::new(&col_descs, 8, num_chunks);
    let total_rows = data.hashes.len();
    let num_batches = (total_rows + BATCH_SIZE - 1) / BATCH_SIZE;

    for batch_idx in 0..num_batches {
        let start = batch_idx * BATCH_SIZE;
        let end = (start + BATCH_SIZE).min(total_rows);
        let batch_hashes = &data.hashes[start..end];
        let batch_values = &data.values[start..end];
        let str_slices: Vec<Vec<&[u8]>> = (0..NUM_STR_COLS)
            .map(|c| data.str_cols[c][start..end].iter().map(|s| s.as_slice()).collect()).collect();
        let mut columns: Vec<ColumnInput> = Vec::new();
        for c in 0..NUM_STR_COLS { columns.push(ColumnInput::Varchar(&str_slices[c])); }
        table.emplace_table_with_decode(batch_hashes, &columns, batch_values);
    }
    table.num_groups()
}

// ═══════════════════════════════════════════════════════════════════
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let sel: f64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(0.1);

    // Second arg: stage filter (A/C/C2/D) or iteration count
    let mut num_iters: usize = 10;
    let mut stage_filter: Option<&str> = None;

    if let Some(arg2) = args.get(2) {
        if arg2.chars().next().map_or(false, |c| c.is_ascii_uppercase()) {
            stage_filter = Some(arg2.as_str());
        } else {
            num_iters = arg2.parse().unwrap_or(10);
        }
    }
    if let Some(arg3) = args.get(3) {
        num_iters = arg3.parse().unwrap_or(10);
    }

    let num_keys = (HT_SIZE as f64 * LOAD_FACTOR) as usize;
    let num_misses = NUM_PROBE_ROWS - (NUM_PROBE_ROWS as f64 * sel) as usize;
    let distinct_keys = num_keys + num_misses;
    let min_slots = ((distinct_keys as f64 / 0.85) as usize).max(8);
    let num_chunks = ((min_slots + 7) / 8).next_power_of_two();

    eprintln!("=== Rust Pipeline Micro Bench ===");
    eprintln!("sel={:.1}, iters={}, totalRows={}, numChunks={}", sel, num_iters, num_keys + NUM_PROBE_ROWS, num_chunks);
    if let Some(s) = stage_filter { eprintln!("Stage filter: {}", s); }
    eprintln!("\nGenerating data...");
    let data = gen_data(num_keys, sel);
    eprintln!("Done.\n");

    let bench = |name: &str, tag: &str, f: fn(&BenchData, usize) -> usize| {
        if let Some(filter) = stage_filter {
            if filter != tag { return; }
        }
        let _ = f(&data, num_chunks); // warmup
        let t0 = Instant::now();
        let mut result = 0;
        for _ in 0..num_iters { result = f(&data, num_chunks); }
        let per_iter = t0.elapsed().as_secs_f64() * 1000.0 / num_iters as f64;
        println!("{:<30}  per_iter={:7.2} ms  result={}", name, per_iter, result);
    };

    println!("=== Results (sel={:.1}, {} iters) ===", sel, num_iters);
    bench("A: hashmap_only", "A", run_hashmap_only);
    bench("C: hashmap+newrow", "C", run_hashmap_plus_newrow);
    bench("C2: hashmap+newrow+serialize", "C2", run_hashmap_plus_serialize);
    bench("D: full_pipeline", "D", run_full_pipeline);
}
