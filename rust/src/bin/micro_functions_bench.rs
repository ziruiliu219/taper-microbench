//! Individual function micro benchmarks — prove each function is equally fast in C++ and Rust.
//!
//! Usage: ./micro_functions_bench [iterations]
//! Default: 10 iterations, 1,000,000 operations each

use std::time::Instant;
use taper_hashmap::column_marshaller::{
    serialize_varchar_to_buffer, compute_row_len_size, compare_varchar_from_row,
    compute_varchar_serialized_size,
};
use taper_hashmap::row_container::{RowContainer, ColumnKind};
use taper_hashmap::taper_hashmap::TaperHashMap;
use taper_hashmap::chunk::SlotValue;

const NUM_OPS: usize = 1_000_000;
const DEFAULT_ITERS: usize = 10;

// ─── 1. arena_alloc_only ─────────────────────────────────────────────────
#[inline(never)]
fn run_arena_alloc_only() -> u64 {
    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);
    let mut checksum: u64 = 0;
    for _ in 0..NUM_OPS {
        let p = rc.arena_alloc(50);
        checksum = checksum.wrapping_add(p as u64);
    }
    checksum
}

// ─── 2. new_row_only ─────────────────────────────────────────────────────
#[inline(never)]
fn run_new_row_only() -> u64 {
    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);
    let mut checksum: u64 = 0;
    for _ in 0..NUM_OPS {
        let row = rc.new_row();
        checksum = checksum.wrapping_add(row as u64);
    }
    checksum
}

// ─── 3. serialize_4str_only ──────────────────────────────────────────────
#[inline(never)]
fn run_serialize_4str_only(str_cols: &[Vec<Vec<u8>>]) -> u64 {
    let key_sizes = vec![0usize; 4];
    let kinds = vec![ColumnKind::Varchar; 4];
    let mut rc = RowContainer::with_kinds(&key_sizes, &kinds, 8);
    let mut checksum: u64 = 0;
    for i in 0..NUM_OPS {
        let mut total_size = 0usize;
        for c in 0..4 {
            let s = &str_cols[c][i];
            total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
        }
        let block = rc.arena_alloc(total_size);
        let mut wp = block;
        for c in 0..4 {
            let s = &str_cols[c][i];
            let written = serialize_varchar_to_buffer(wp, s.as_slice());
            wp = unsafe { wp.add(written) };
        }
        checksum = checksum.wrapping_add(block as u64);
    }
    checksum
}

// ─── 4. compare_4str_only ────────────────────────────────────────────────
#[inline(never)]
fn run_compare_4str_only(arena_blocks: &[*const u8], str_cols: &[Vec<Vec<u8>>]) -> u64 {
    let mut match_count: u64 = 0;
    for i in 0..NUM_OPS {
        let mut pos = arena_blocks[i];
        let mut all_match = true;
        for c in 0..4 {
            let s = &str_cols[c][i];
            if !compare_varchar_from_row(pos, s.as_slice()) {
                all_match = false;
                break;
            }
            let entry_size = compute_varchar_serialized_size(pos);
            pos = unsafe { pos.add(entry_size) };
        }
        if all_match { match_count += 1; }
    }
    match_count
}

// ─── 5. hashmap_probe_only ───────────────────────────────────────────────
#[inline(never)]
fn run_hashmap_probe_only(hashes: &[u64], num_chunks: usize) -> u64 {
    let mut table = TaperHashMap::with_capacity(num_chunks);
    let mut sum: u64 = 0;
    table.emplace_batch_full(
        hashes,
        &|_: usize, _: &SlotValue| -> bool { true },
        &mut |_i: usize, sv: &mut SlotValue| { sv.bytes = [0x42; 6]; },
        &mut |_i: usize, _sv: &SlotValue, is_new: bool| { if !is_new { sum += 1; } },
    );
    table.len() as u64 + sum
}

// ─── 6. setrowptr_only (set_ptr / get_ptr) ──────────────────────────────
#[inline(never)]
fn run_set_get_ptr_only() -> u64 {
    let mut sv = SlotValue { bytes: [0u8; 6] };
    let mut checksum: u64 = 0;
    for i in 0..NUM_OPS {
        let ptr = (0x7FFF00000000u64 + (i as u64) * 41) as *const u8;
        sv.set_ptr(ptr);
        let got = sv.get_ptr();
        checksum = checksum.wrapping_add(got as u64);
    }
    checksum
}

// ═══════════════════════════════════════════════════════════════════
fn main() {
    let num_iters: usize = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(DEFAULT_ITERS);

    eprintln!("Generating data...");

    // String data
    let str_cols: Vec<Vec<Vec<u8>>> = (0..4)
        .map(|c| {
            (0..NUM_OPS)
                .map(|i| format!("key_{}_c{}", i, c).into_bytes())
                .collect()
        })
        .collect();

    // Pre-serialize for compare bench — rc must live as long as arena_blocks
    let key_sizes_cmp = vec![0usize; 4];
    let kinds_cmp = vec![ColumnKind::Varchar; 4];
    let mut rc_for_compare = RowContainer::with_kinds(&key_sizes_cmp, &kinds_cmp, 8);
    let arena_blocks: Vec<*const u8> = (0..NUM_OPS)
        .map(|i| {
            let mut total_size = 0usize;
            for c in 0..4 {
                let s = &str_cols[c][i];
                total_size += 1 + compute_row_len_size(s.len()) as usize + s.len();
            }
            let block = rc_for_compare.arena_alloc(total_size);
            let mut wp = block;
            for c in 0..4 {
                let s = &str_cols[c][i];
                let written = serialize_varchar_to_buffer(wp, s.as_slice());
                wp = unsafe { wp.add(written) };
            }
            block as *const u8
        })
        .collect();

    // Hashes for probe bench
    use rand_mt::Mt19937GenRand64;
    let mut rng = Mt19937GenRand64::new(42);
    let hashes: Vec<u64> = (0..NUM_OPS).map(|_| rng.next_u64()).collect();
    let mut num_chunks = 1usize;
    while num_chunks * 8 < NUM_OPS { num_chunks *= 2; }

    eprintln!("Done. Running {} iters...\n", num_iters);

    let bench = |name: &str, f: &dyn Fn() -> u64| {
        let _ = f(); // warmup
        let t0 = Instant::now();
        let mut checksum: u64 = 0;
        for _ in 0..num_iters { checksum = f(); }
        let per_iter = t0.elapsed().as_secs_f64() * 1000.0 / num_iters as f64;
        println!("{:<25}  per_iter={:7.3} ms  checksum={}", name, per_iter, checksum);
    };

    println!("=== Rust Individual Function Bench ({} ops, {} iters) ===", NUM_OPS, num_iters);
    bench("1. arena_alloc_only", &|| run_arena_alloc_only());
    bench("2. new_row_only", &|| run_new_row_only());
    bench("3. serialize_4str_only", &|| run_serialize_4str_only(&str_cols));
    bench("4. compare_4str_only", &|| run_compare_4str_only(&arena_blocks, &str_cols));
    bench("5. hashmap_probe_only", &|| run_hashmap_probe_only(&hashes, num_chunks));
    bench("6. setrowptr_only", &|| run_set_get_ptr_only());
}
