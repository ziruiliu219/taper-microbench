/// Demo: serialize 4 short strings WITH bump-pointer arena allocation per row.
/// Uses libc::memcpy + libc::malloc — identical system calls as C++.
///
/// Build: cargo build --release --bin rust_serialize_arena_demo
/// Run:   taskset -c 10 ./target/release/rust_serialize_arena_demo

use std::time::Instant;

const NUM_ROWS: usize = 1_000_000;
const NUM_COLS: usize = 4;
const NUM_ITERS: usize = 10;

// Simple bump-pointer arena — same logic as C++ Arena / OmniOperator SimpleArenaAllocator
struct Arena {
    chunks: Vec<(*mut u8, usize)>,
    buf: *mut u8,
    avail: usize,
    min_chunk: usize,
    growth_factor: usize,
    linear_threshold: usize,
}

impl Arena {
    fn new() -> Self {
        Arena {
            chunks: Vec::new(), buf: std::ptr::null_mut(), avail: 0,
            min_chunk: 4096, growth_factor: 2, linear_threshold: 512 * 1024,
        }
    }
    fn next_size(&self, needed: usize) -> usize {
        match self.chunks.last() {
            None => needed.max(self.min_chunk),
            Some(&(_, last_size)) if last_size < self.linear_threshold => needed.max(last_size * self.growth_factor),
            _ => ((needed + self.linear_threshold - 1) / self.linear_threshold) * self.linear_threshold,
        }
    }
    fn alloc_chunk(&mut self, size: usize) {
        let ptr = unsafe { libc::malloc(size) as *mut u8 };
        assert!(!ptr.is_null());
        self.chunks.push((ptr, size));
        self.buf = ptr;
        self.avail = size;
    }
    fn allocate(&mut self, size: usize) -> *mut u8 {
        if self.avail < size { self.alloc_chunk(self.next_size(size)); }
        let ret = self.buf;
        self.buf = unsafe { self.buf.add(size) };
        self.avail -= size;
        ret
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        for &(ptr, _) in &self.chunks {
            unsafe { libc::free(ptr as *mut libc::c_void); }
        }
    }
}

// SerializeVarcharToBuffer — uses libc::memcpy, identical to C++
#[inline]
fn serialize_one(dst: *mut u8, data: &[u8]) -> usize {
    let len = data.len();
    let row_len_size: u8 = if len <= 0xFF { 1 } else if len <= 0xFFFF { 2 } else { 4 };
    unsafe {
        *dst = row_len_size;
        let l32 = len as u32;
        libc::memcpy(
            dst.add(1) as *mut libc::c_void,
            &l32 as *const u32 as *const libc::c_void,
            row_len_size as usize,
        );
        if len > 0 {
            libc::memcpy(
                dst.add(1 + row_len_size as usize) as *mut libc::c_void,
                data.as_ptr() as *const libc::c_void,
                len,
            );
        }
    }
    1 + row_len_size as usize + len
}

// Hot loop: arena allocate + serialize 4 columns per row
#[inline(never)]
fn bench_serialize_arena(cols: &[Vec<&[u8]>]) -> u64 {
    let mut arena = Arena::new();
    let num_cols = std::hint::black_box(NUM_COLS);
    let mut checksum: u64 = 0;
    for i in 0..NUM_ROWS {
        // Compute total size
        let mut total = 0usize;
        for c in 0..num_cols {
            let len = cols[c][i].len();
            let rls: usize = if len <= 0xFF { 1 } else if len <= 0xFFFF { 2 } else { 4 };
            total += 1 + rls + len;
        }
        // Arena allocate
        let block = arena.allocate(total);
        let mut wp = block;
        // Serialize 4 columns
        for c in 0..num_cols {
            let written = serialize_one(wp, cols[c][i]);
            wp = unsafe { wp.add(written) };
        }
        checksum = checksum.wrapping_add(block as u64);
    }
    checksum
}

fn main() {
    // Generate data
    let str_data: Vec<Vec<String>> = (0..NUM_COLS)
        .map(|c| (0..NUM_ROWS).map(|i| format!("key_{}_c{}", i, c)).collect())
        .collect();
    let cols: Vec<Vec<&[u8]>> = str_data.iter()
        .map(|col| col.iter().map(|s| s.as_bytes()).collect())
        .collect();

    // Warmup
    let _ = bench_serialize_arena(&cols);

    // Measure
    let t0 = Instant::now();
    let mut result: u64 = 0;
    for _ in 0..NUM_ITERS {
        result = bench_serialize_arena(&cols);
    }
    let ms = t0.elapsed().as_secs_f64() * 1000.0 / NUM_ITERS as f64;

    println!("Rust serialize+arena (libc::memcpy): {:.2} ms  checksum={}", ms, result);
}
