//! Micro benchmarks to isolate Rust vs C++ performance in row/string paths.
//!
//! Usage: ./micro_cases_bench [--case serialize_only|compare_only|hashmap_plus_empty_row] [iterations]
//!
//! Default: all cases, 10 iterations

use std::time::Instant;
use taper_hashmap::column_marshaller::{
    serialize_varchar_to_buffer, compute_row_len_size, compare_varchar_from_row,
    compute_varchar_serialized_size,
};
use taper_hashmap::row_container::RowContainer;
use taper_hashmap::taper_hashmap::TaperHashMap;
use taper_hashmap::chunk::SlotValue;

const NUM_ROWS: usize = 1_000_000;
const NUM_STR_COLS: usize = 4;
const DEFAULT_ITERS: usize = 10;

// ═══════════════════════════════════════════════════════════════════
// Data generation
// ═══════════════════════════════════════════════════════════════════

struct TestData {
    str_cols: Vec<Vec<Vec<u8>>>, // [col][row]
    hashes: Vec<u64>,
}

fn gen_test_data() -> TestData {
    use rand_mt::Mt19937GenRand64;
    let mut rng = Mt19937GenRand64::new(42);

    let str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| {
            (0..NUM_ROWS)
                .map(|i| format!("key_{}_c{}", i, c).into_bytes())
                .collect()
        })
        .collect();

    let hashes: Vec<u64> = (0..NUM_ROWS).map(|_| rng.next_u64()).collect();

    TestData { str_cols, hashes }
}

// ═══════════════════════════════════════════════════════════════════
// Case 1: serialize_only
// ═══════════════════════════════════════════════════════════════════

#[inline(never)]
fn run_serialize_only(data: &TestData) -> u64 {
    use taper_hashmap::row_container::ColumnKind;

    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);

    let agg_offset = rc.agg_state_offset();
    let varchar_slot_offset = rc.column_at(0).offset();
    let mut checksum: u64 = 0;

    for i in 0..NUM_ROWS {
        let row = rc.new_row();

        // Serialize 4 varchars into one merged block
        let mut total_size = 0usize;
        for c in 0..NUM_STR_COLS {
            let s = &data.str_cols[c][i];
            total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
        }
        let block = rc.arena_alloc(total_size);
        let mut wp = block;
        for c in 0..NUM_STR_COLS {
            let s = &data.str_cols[c][i];
            let written = unsafe { serialize_varchar_to_buffer(wp, s.as_slice()) };
            wp = unsafe { wp.add(written) };
        }
        // Store pointer in slot column
        unsafe { (row.add(varchar_slot_offset) as *mut *const u8).write_unaligned(block as *const u8); }

        // Store agg value
        let val = (i % 1000) as i64;
        RowContainer::store_value::<i64>(row, agg_offset, val);
        checksum += val as u64;
    }

    checksum + rc.num_rows() as u64
}

// ═══════════════════════════════════════════════════════════════════
// Case 2: compare_only
// ═══════════════════════════════════════════════════════════════════

#[inline(never)]
fn run_compare_only(data: &TestData) -> u64 {
    use taper_hashmap::row_container::ColumnKind;

    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);

    let varchar_slot_offset = rc.column_at(0).offset();
    let mut arena_blocks: Vec<*const u8> = Vec::with_capacity(NUM_ROWS);

    // First serialize all rows
    for i in 0..NUM_ROWS {
        let row = rc.new_row();
        let mut total_size = 0usize;
        for c in 0..NUM_STR_COLS {
            let s = &data.str_cols[c][i];
            total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
        }
        let block = rc.arena_alloc(total_size);
        let mut wp = block;
        for c in 0..NUM_STR_COLS {
            let s = &data.str_cols[c][i];
            let written = unsafe { serialize_varchar_to_buffer(wp, s.as_slice()) };
            wp = unsafe { wp.add(written) };
        }
        unsafe { (row.add(varchar_slot_offset) as *mut *const u8).write_unaligned(block as *const u8); }
        arena_blocks.push(block as *const u8);
    }

    // Now compare (100% equal)
    let mut match_count: u64 = 0;
    for i in 0..NUM_ROWS {
        let mut pos = arena_blocks[i];
        let mut all_match = true;
        for c in 0..NUM_STR_COLS {
            let s = &data.str_cols[c][i];
            if !compare_varchar_from_row(pos, s.as_slice()) {
                all_match = false;
                break;
            }
            let entry_size = unsafe { compute_varchar_serialized_size(pos) };
            pos = unsafe { pos.add(entry_size) };
        }
        if all_match { match_count += 1; }
    }

    match_count
}

// ═══════════════════════════════════════════════════════════════════
// Case 3: hashmap_plus_empty_row
// ═══════════════════════════════════════════════════════════════════

#[inline(never)]
fn run_hashmap_plus_empty_row(data: &TestData) -> u64 {
    let mut num_chunks = 1usize;
    while num_chunks * 8 < (NUM_ROWS as f64 / 0.85) as usize { num_chunks *= 2; }

    let mut table = TaperHashMap::with_capacity(num_chunks);

    // Row layout: 4*8 ptr slots + 1 null byte + 8 agg = 41 bytes
    const ROW_SIZE: usize = 41;
    const AGG_OFFSET: usize = 33;
    const BATCH_BLOCK: usize = 1024;

    // Simple arena for rows
    let mut blocks: Vec<Vec<u8>> = Vec::new();
    let mut block_pos: usize = BATCH_BLOCK; // force first allocation

    let mut checksum: u64 = 0;
    let mut row_ptrs: Vec<*mut u8> = Vec::new();

    table.emplace_batch_full(
        &data.hashes,
        &|_: usize, _: &SlotValue| -> bool { true },
        &mut |i: usize, sv: &mut SlotValue| {
            // Allocate row
            if block_pos >= BATCH_BLOCK {
                let mut block = vec![0u8; ROW_SIZE * BATCH_BLOCK];
                row_ptrs.push(block.as_mut_ptr());
                blocks.push(block);
                block_pos = 0;
            }
            let row = unsafe { blocks.last_mut().unwrap().as_mut_ptr().add(block_pos * ROW_SIZE) };
            block_pos += 1;

            // Write agg
            let val = (i % 1000) as i64;
            unsafe { (row.add(AGG_OFFSET) as *mut i64).write_unaligned(val); }

            // Store pointer
            sv.set_ptr(row as *const u8);
        },
        &mut |_i: usize, sv: &SlotValue, is_new: bool| {
            if !is_new {
                let row = sv.get_ptr() as *const u8;
                let val: i64 = unsafe { (row.add(AGG_OFFSET) as *const i64).read_unaligned() };
                checksum += val as u64;
            }
        },
    );

    table.len() as u64 + checksum
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut num_iters = DEFAULT_ITERS;
    let mut case_filter: Option<&str> = None;

    let mut i = 1;
    while i < args.len() {
        if args[i] == "--case" && i + 1 < args.len() {
            case_filter = Some(&args[i + 1]);
            i += 2;
        } else {
            num_iters = args[i].parse().unwrap_or(DEFAULT_ITERS);
            i += 1;
        }
    }

    eprintln!("Generating test data ({} rows, {} str cols)...", NUM_ROWS, NUM_STR_COLS);
    let data = gen_test_data();
    eprintln!("Done.\n");

    let run_case = |name: &str, f: fn(&TestData) -> u64| {
        if let Some(filter) = case_filter {
            if filter != name { return; }
        }
        eprintln!("--- {} (warmup) ---", name);
        let _ = f(&data);

        eprintln!("--- {} ({} iters) ---", name, num_iters);
        let t0 = Instant::now();
        let mut checksum: u64 = 0;
        for _ in 0..num_iters {
            checksum = f(&data);
        }
        let elapsed = t0.elapsed();
        let total_ms = elapsed.as_secs_f64() * 1000.0;
        let per_iter_ms = total_ms / num_iters as f64;

        println!("{:<25}  total={:8.1} ms  per_iter={:7.3} ms  checksum={}",
            name, total_ms, per_iter_ms, checksum);
    };

    println!("=== Rust Micro Cases (rows={}, iters={}) ===", NUM_ROWS, num_iters);
    run_case("serialize_only", run_serialize_only);
    run_case("compare_only", run_compare_only);
    run_case("hashmap_plus_empty_row", run_hashmap_plus_empty_row);
}
