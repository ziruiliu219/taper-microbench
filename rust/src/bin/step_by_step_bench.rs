//! Step-by-step function benchmark — tests every function in the EmplaceTableWithDecode pipeline.
//!
//! Usage: ./step_by_step_bench <sel> [iterations]
//! Default: sel=0.1, 10 iterations, 1M rows per iteration

use std::time::Instant;
use taper_hashmap::column_marshaller::{
    TaperColumnSerializeHandler, ColumnDesc, ColumnInput,
    serialize_varchar_to_buffer, compute_row_len_size, compare_varchar_from_row,
    compute_varchar_serialized_size,
};
use taper_hashmap::row_container::{RowContainer, ColumnKind};
use xxhash_rust::xxh3::xxh3_64_with_seed;
use rand_mt::Mt19937GenRand64;

const NUM_STR_COLS: usize = 4;
const NUM_PROBE_ROWS: usize = 1_000_000;
const BATCH_SIZE: usize = 410;
const SEED: u64 = 42;
const DEFAULT_ITERS: usize = 10;

fn hash_bytes(data: &[u8], seed: u64) -> u64 { xxh3_64_with_seed(data, seed) }
fn gen_string(base: &str, id: usize, col: usize) -> Vec<u8> { format!("{}_{}_c{}", base, id, col).into_bytes() }

struct TestData {
    str_cols: Vec<Vec<Vec<u8>>>,
    hashes: Vec<u64>,
    values: Vec<i64>,
    total_rows: usize,
    num_keys: usize,
    num_chunks: usize,
}

fn gen_data(sel: f64, ht_size: usize, load_factor: f64) -> TestData {
    let num_keys = (ht_size as f64 * load_factor) as usize;
    let mut rng = Mt19937GenRand64::new(SEED);
    let mut str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| (0..num_keys).map(|i| gen_string("key", i, c)).collect()).collect();
    let build_hashes: Vec<u64> = (0..num_keys).map(|i| {
        let mut h = 0u64;
        for c in 0..NUM_STR_COLS { h = hash_bytes(&str_cols[c][i], h); }
        h
    }).collect();

    let num_hits = (NUM_PROBE_ROWS as f64 * sel) as usize;
    let num_misses = NUM_PROBE_ROWS - num_hits;
    let mut probe_str: Vec<Vec<Vec<u8>>> = vec![Vec::new(); NUM_STR_COLS];
    let mut probe_hashes: Vec<u64> = Vec::new();
    for _ in 0..num_hits { let idx = (rng.next_u64() as usize) % num_keys; for c in 0..NUM_STR_COLS { probe_str[c].push(str_cols[c][idx].clone()); } probe_hashes.push(build_hashes[idx]); }
    for i in 0..num_misses { let mut h = 0u64; for c in 0..NUM_STR_COLS { let s = format!("miss_{}_{}", i, c).into_bytes(); h = hash_bytes(&s, h); probe_str[c].push(s); } probe_hashes.push(h); }

    let mut order: Vec<usize> = (0..NUM_PROBE_ROWS).collect();
    for i in (1..NUM_PROBE_ROWS).rev() { order.swap(i, (rng.next_u64() as usize) % (i + 1)); }
    let probe_str: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS).map(|c| order.iter().map(|&i| probe_str[c][i].clone()).collect()).collect();
    let probe_hashes: Vec<u64> = order.iter().map(|&i| probe_hashes[i]).collect();

    let total_rows = num_keys + NUM_PROBE_ROWS;
    for c in 0..NUM_STR_COLS { str_cols[c].extend(probe_str[c].iter().cloned()); }
    let mut all_hashes = build_hashes; all_hashes.extend_from_slice(&probe_hashes);
    let values: Vec<i64> = (0..total_rows).map(|i| (i % 1000) as i64).collect();

    let distinct_keys = num_keys + num_misses;
    let min_slots = ((distinct_keys as f64 / 0.85) as usize).max(8);
    let num_chunks = ((min_slots + 7) / 8).next_power_of_two();

    TestData { str_cols, hashes: all_hashes, values, total_rows, num_keys, num_chunks }
}

// ═══════════════════════════════════════════════════════════════════

#[inline(never)]
fn bench_precompute_positions(d: &TestData) -> u64 {
    let mask = d.num_chunks - 1;
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let pos = (d.hashes[i] as usize) & mask;
        checksum = checksum.wrapping_add(pos as u64);
    }
    checksum
}

#[inline(never)]
fn bench_new_row(d: &TestData) -> u64 {
    let ks = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&ks, &kinds, 8);
    let mut checksum: u64 = 0;
    for _ in 0..d.total_rows {
        let row = rc.new_row();
        checksum = checksum.wrapping_add(row as u64);
    }
    checksum
}

#[inline(never)]
fn bench_serialize(d: &TestData) -> u64 {
    let ks = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&ks, &kinds, 8);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let mut total_size = 0usize;
        for c in 0..NUM_STR_COLS {
            let s = &d.str_cols[c][i];
            total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
        }
        let block = rc.arena_alloc(total_size);
        let mut wp = block;
        for c in 0..NUM_STR_COLS {
            let s = &d.str_cols[c][i];
            let written = serialize_varchar_to_buffer(wp, s.as_slice());
            wp = unsafe { wp.add(written) };
        }
        checksum = checksum.wrapping_add(block as u64);
    }
    checksum
}

#[inline(never)]
fn bench_store_value(d: &TestData) -> u64 {
    let ks = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&ks, &kinds, 8);
    let agg_offset = rc.agg_state_offset();
    let mut rows: Vec<*mut u8> = Vec::with_capacity(d.total_rows);
    for _ in 0..d.total_rows { rows.push(rc.new_row()); }

    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        RowContainer::store_value::<i64>(rows[i], agg_offset, d.values[i]);
        checksum = checksum.wrapping_add(d.values[i] as u64);
    }
    checksum
}

#[inline(never)]
fn bench_compare_varchar(d: &TestData) -> u64 {
    // Pre-serialize
    let ks = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&ks, &kinds, 8);
    let mut blocks: Vec<*const u8> = Vec::with_capacity(d.total_rows);
    for i in 0..d.total_rows {
        let mut total_size = 0usize;
        for c in 0..NUM_STR_COLS {
            let s = &d.str_cols[c][i];
            total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
        }
        let block = rc.arena_alloc(total_size);
        let mut wp = block;
        for c in 0..NUM_STR_COLS {
            let s = &d.str_cols[c][i];
            let written = serialize_varchar_to_buffer(wp, s.as_slice());
            wp = unsafe { wp.add(written) };
        }
        blocks.push(block as *const u8);
    }

    // Compare (100% equal)
    let mut match_count: u64 = 0;
    for i in 0..d.total_rows {
        let mut pos = blocks[i];
        let mut all_match = true;
        for c in 0..NUM_STR_COLS {
            let s = &d.str_cols[c][i];
            if !compare_varchar_from_row(pos, s.as_slice()) { all_match = false; break; }
            let entry_size = compute_varchar_serialized_size(pos);
            pos = unsafe { pos.add(entry_size) };
        }
        if all_match { match_count += 1; }
    }
    match_count
}

#[inline(never)]
fn bench_accumulate(d: &TestData) -> u64 {
    let ks = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&ks, &kinds, 8);
    let agg_offset = rc.agg_state_offset();
    let mut rows: Vec<*mut u8> = Vec::with_capacity(d.num_keys);
    for _ in 0..d.num_keys {
        let row = rc.new_row();
        RowContainer::store_value::<i64>(row, agg_offset, 0i64);
        rows.push(row);
    }

    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let row = rows[i % d.num_keys];
        let p = unsafe { &mut *(row.add(agg_offset) as *mut i64) };
        *p += d.values[i];
        checksum = checksum.wrapping_add(*p as u64);
    }
    checksum
}

#[inline(never)]
fn bench_full_pipeline(d: &TestData) -> u64 {
    let col_descs = vec![ColumnDesc::Varchar; NUM_STR_COLS];
    let mut table = TaperColumnSerializeHandler::new(&col_descs, 8, d.num_chunks);
    let num_batches = (d.total_rows + BATCH_SIZE - 1) / BATCH_SIZE;
    for batch_idx in 0..num_batches {
        let start = batch_idx * BATCH_SIZE;
        let end = (start + BATCH_SIZE).min(d.total_rows);
        let batch_hashes = &d.hashes[start..end];
        let batch_values = &d.values[start..end];
        let str_slices: Vec<Vec<&[u8]>> = (0..NUM_STR_COLS)
            .map(|c| d.str_cols[c][start..end].iter().map(|s| s.as_slice()).collect()).collect();
        let mut columns: Vec<ColumnInput> = Vec::new();
        for c in 0..NUM_STR_COLS { columns.push(ColumnInput::Varchar(&str_slices[c])); }
        table.emplace_table_with_decode(batch_hashes, &columns, batch_values);
    }
    table.num_groups() as u64
}

// ═══════════════════════════════════════════════════════════════════
// Usage: ./step_by_step_bench <sel> [iters] [ht_size] [load_factor]
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let sel: f64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(0.1);
    let num_iters: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_ITERS);
    let ht_size: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(16384);
    let load_factor: f64 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(0.5);

    eprintln!("=== Rust Step-by-Step Bench ===");
    eprintln!("sel={:.2}, iters={}, ht={}, lf={:.2}", sel, num_iters, ht_size, load_factor);
    eprintln!("Generating data...");
    let data = gen_data(sel, ht_size, load_factor);
    eprintln!("totalRows={}, numKeys={}, numChunks={}\n", data.total_rows, data.num_keys, data.num_chunks);

    let bench = |name: &str, f: fn(&TestData) -> u64| {
        let _ = f(&data); // warmup
        let t0 = Instant::now();
        let mut checksum: u64 = 0;
        for _ in 0..num_iters { checksum = f(&data); }
        let per_iter = t0.elapsed().as_secs_f64() * 1000.0 / num_iters as f64;
        println!("{:<30}  {:7.2} ms  checksum={}", name, per_iter, checksum);
    };

    println!("=== Rust Step-by-Step (ht={}, lf={:.2}, sel={:.2}, {} iters, {} rows) ===", ht_size, load_factor, sel, num_iters, data.total_rows);
    bench("1. precompute_positions", bench_precompute_positions);
    bench("5. new_row", bench_new_row);
    bench("7. serialize_4str", bench_serialize);
    bench("8. store_value_i64", bench_store_value);
    bench("10. compare_varchar_4col", bench_compare_varchar);
    bench("12. accumulate_equals", bench_accumulate);
    bench("FULL: pipeline", bench_full_pipeline);
}
