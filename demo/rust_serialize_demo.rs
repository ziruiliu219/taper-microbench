/// Minimal demo: serialize 4 short strings into a buffer, 1M iterations.
/// Proves that rustc keeps serialize_one as small function → less stall on Kunpeng.
///
/// Build: rustc -O -o rust_serialize_demo rust_serialize_demo.rs
/// Run:   taskset -c 10 ./rust_serialize_demo

use std::time::Instant;

const NUM_ROWS: usize = 1_000_000;
const NUM_COLS: usize = 4;
const NUM_ITERS: usize = 10;

// Mirrors OmniOperator's SerializeVarcharToBuffer — uses memcpy via copy_nonoverlapping
#[inline]
fn serialize_one(dst: *mut u8, data: &[u8]) -> usize {
    let len = data.len();
    let row_len_size: u8 = if len <= 0xFF { 1 } else if len <= 0xFFFF { 2 } else { 4 };
    unsafe {
        *dst = row_len_size;
        let l32 = len as u32;
        std::ptr::copy_nonoverlapping(
            &l32 as *const u32 as *const u8,
            dst.add(1),
            row_len_size as usize,
        );
        if len > 0 {
            std::ptr::copy_nonoverlapping(
                data.as_ptr(),
                dst.add(1 + row_len_size as usize),
                len,
            );
        }
    }
    1 + row_len_size as usize + len
}

#[inline(never)]
fn bench_serialize(cols: &[Vec<&[u8]>], arena: &mut [u8]) -> u64 {
    let num_cols = std::hint::black_box(NUM_COLS);
    let mut checksum: u64 = 0;
    let mut offset: usize = 0;
    let arena_size = arena.len();
    for i in 0..NUM_ROWS {
        // Compute total size
        let mut total = 0usize;
        for c in 0..num_cols {
            let len = cols[c][i].len();
            let rls: usize = if len <= 0xFF { 1 } else if len <= 0xFFFF { 2 } else { 4 };
            total += 1 + rls + len;
        }
        // Reset if near end
        if offset + total > arena_size { offset = 0; }
        let block = unsafe { arena.as_mut_ptr().add(offset) };
        let mut wp = block;
        // Serialize 4 columns
        for c in 0..num_cols {
            let written = serialize_one(wp, cols[c][i]);
            wp = unsafe { wp.add(written) };
        }
        offset += total;
        checksum = checksum.wrapping_add(block as u64);
    }
    checksum
}

fn main() {
    // Generate data: 4 columns of ~12 byte strings
    let str_data: Vec<Vec<String>> = (0..NUM_COLS)
        .map(|c| (0..NUM_ROWS).map(|i| format!("key_{}_c{}", i, c)).collect())
        .collect();
    let cols: Vec<Vec<&[u8]>> = str_data.iter()
        .map(|col| col.iter().map(|s| s.as_bytes()).collect())
        .collect();

    // Pre-allocate arena (64MB)
    let mut arena = vec![0u8; 64 * 1024 * 1024];

    // Warmup
    let _ = bench_serialize(&cols, &mut arena);

    // Measure
    let t0 = Instant::now();
    let mut result: u64 = 0;
    for _ in 0..NUM_ITERS {
        result = bench_serialize(&cols, &mut arena);
    }
    let ms = t0.elapsed().as_secs_f64() * 1000.0 / NUM_ITERS as f64;

    println!("Rust serialize_demo: {:.2} ms  checksum={}", ms, result);
}
