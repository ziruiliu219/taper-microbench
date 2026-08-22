//! EmplaceBatch Breakdown Bench — times every sub-function in the hash agg pipeline.
//!
//! Usage: ./emplace_breakdown_bench <sel> [iters] [ht_size] [load_factor]

use std::time::Instant;
use taper_hashmap::column_marshaller::{
    TaperColumnSerializeHandler, ColumnDesc, ColumnInput,
    serialize_varchar_to_buffer, compute_row_len_size, compare_varchar_from_row,
    compute_varchar_serialized_size,
    serialize_varchar_to_buffer_noinline_memcpy,
};
use taper_hashmap::row_container::{RowContainer, ColumnKind};
use taper_hashmap::taper_hashmap::TaperHashMap;
use taper_hashmap::bitmask::BitMask;
use taper_hashmap::chunk::Chunk;
use xxhash_rust::xxh3::xxh3_64_with_seed;
use rand_mt::Mt19937GenRand64;

const NUM_STR_COLS: usize = 4;
const NUM_PROBE_ROWS: usize = 1_000_000;
const BATCH_SIZE: usize = 410;
const SEED: u64 = 42;
const DEFAULT_ITERS: usize = 10;

fn hash_bytes(data: &[u8], seed: u64) -> u64 { xxh3_64_with_seed(data, seed) }

/// Mirrors C++ VarcharSlice { ptr, len } — 16 bytes, same layout
#[repr(C)]
#[derive(Clone, Copy)]
struct Slice {
    ptr: *const u8,
    len: usize,
}

struct TestData {
    str_cols: Vec<Vec<Vec<u8>>>,
    slices: Vec<Vec<Slice>>,  // pre-computed (ptr, len) pairs — same as C++
    flat_slices: Vec<Slice>,  // flat_slices[i * NUM_STR_COLS + c]
    hashes: Vec<u64>,
    values: Vec<i64>,
    total_rows: usize,
    num_keys: usize,
    num_chunks: usize,
}

fn gen_data(sel: f64, ht_size: usize) -> TestData {
    // ─── Two params only: ht_size + sel ───
    let num_chunks = (ht_size / 8).max(1).next_power_of_two();
    let capacity = num_chunks * 8;
    // distinctKeys = expandThreshold - 1, matching C++ integer arithmetic exactly.
    // C++ threshold: capacity * 9 / 10. We stay 1 below → impossible to trigger expand.
    let expand_threshold = capacity * 9 / 10;
    let distinct_keys = (expand_threshold - 1).max(1);

    // sel controls build/probe split:
    //   numKeys = distinctKeys * sel (inserted during build)
    //   probeMisses = distinctKeys - numKeys (new keys during probe)
    let num_keys = ((distinct_keys as f64 * sel) as usize).max(1);
    let probe_misses = distinct_keys - num_keys;
    let probe_hits = if NUM_PROBE_ROWS > probe_misses { NUM_PROBE_ROWS - probe_misses } else { 0 };
    let num_probe_rows = probe_hits + probe_misses;

    eprintln!("  sizing: numChunks={}, capacity={}, distinctKeys={}", num_chunks, capacity, distinct_keys);
    eprintln!("  build: numKeys={} ({:.1}% of capacity)", num_keys, 100.0 * num_keys as f64 / capacity as f64);
    eprintln!("  probe: hits={}, misses={}, probeRows={}", probe_hits, probe_misses, num_probe_rows);
    eprintln!("  final fill: {}/{} = {:.1}% (expand threshold=90.0%)",
             distinct_keys, capacity, 100.0 * distinct_keys as f64 / capacity as f64);

    let mut rng = Mt19937GenRand64::new(SEED);
    let mut str_cols: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS)
        .map(|c| (0..num_keys).map(|i| format!("key_{}_c{}", i, c).into_bytes()).collect()).collect();
    let build_hashes: Vec<u64> = (0..num_keys).map(|i| {
        let mut h = 0u64;
        for c in 0..NUM_STR_COLS { h = hash_bytes(&str_cols[c][i], h); }
        h
    }).collect();

    let mut probe_str: Vec<Vec<Vec<u8>>> = vec![Vec::new(); NUM_STR_COLS];
    let mut probe_hashes: Vec<u64> = Vec::new();
    for _ in 0..probe_hits {
        let idx = (rng.next_u64() as usize) % num_keys;
        for c in 0..NUM_STR_COLS { probe_str[c].push(str_cols[c][idx].clone()); }
        probe_hashes.push(build_hashes[idx]);
    }
    for i in 0..probe_misses {
        let mut h = 0u64;
        for c in 0..NUM_STR_COLS {
            let s = format!("miss_{}_{}", i, c).into_bytes();
            h = hash_bytes(&s, h);
            probe_str[c].push(s);
        }
        probe_hashes.push(h);
    }

    // Shuffle probe rows
    let mut order: Vec<usize> = (0..num_probe_rows).collect();
    for i in (1..num_probe_rows).rev() { order.swap(i, (rng.next_u64() as usize) % (i + 1)); }
    let probe_str: Vec<Vec<Vec<u8>>> = (0..NUM_STR_COLS).map(|c| order.iter().map(|&i| probe_str[c][i].clone()).collect()).collect();
    let probe_hashes: Vec<u64> = order.iter().map(|&i| probe_hashes[i]).collect();

    // Combine: base keys first, then probe rows
    let total_rows = num_keys + num_probe_rows;
    for c in 0..NUM_STR_COLS { str_cols[c].extend(probe_str[c].iter().cloned()); }
    let mut all_hashes = build_hashes; all_hashes.extend_from_slice(&probe_hashes);
    let values: Vec<i64> = (0..total_rows).map(|i| (i % 1000) as i64).collect();

    // Pre-compute slices
    let slices: Vec<Vec<Slice>> = (0..NUM_STR_COLS)
        .map(|c| str_cols[c].iter().map(|s| Slice { ptr: s.as_ptr(), len: s.len() }).collect())
        .collect();

    // Flat slices: row-major [i * NUM_STR_COLS + c]
    let mut flat_slices: Vec<Slice> = Vec::with_capacity(total_rows * NUM_STR_COLS);
    for i in 0..total_rows {
        for c in 0..NUM_STR_COLS {
            flat_slices.push(Slice { ptr: str_cols[c][i].as_ptr(), len: str_cols[c][i].len() });
        }
    }

    // Debug: print len checksum to verify data matches C++
    {
        let len_sum: u64 = flat_slices.iter().map(|s| s.len as u64).sum();
        eprint!("  flatSlices lenSum={}, first10:", len_sum);
        for i in 0..10.min(flat_slices.len()) { eprint!(" {}", flat_slices[i].len); }
        eprintln!();
    }

    TestData { str_cols, slices, flat_slices, hashes: all_hashes, values, total_rows, num_keys, num_chunks }
}

// ─── Steps ───────────────────────────────────────────────────────

#[inline(never)]
fn bench_hash_and_position(d: &TestData) -> u64 {
    let table = TaperHashMap::with_capacity(d.num_chunks);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let hv = TaperHashMap::bench_hash(d.hashes[i]);
        let pos = table.bench_chunk_pos(hv);
        checksum = checksum.wrapping_add(pos as u64);
    }
    checksum
}

#[inline(never)]
fn bench_load_tags(d: &TestData) -> u64 {
    let table = TaperHashMap::with_capacity(d.num_chunks);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let hv = TaperHashMap::bench_hash(d.hashes[i]);
        let pos = table.bench_chunk_pos(hv);
        let t = table.bench_chunk_at(pos).tags_u64();
        checksum = checksum.wrapping_add(t);
    }
    checksum
}

#[inline(never)]
fn bench_match_tag(d: &TestData) -> u64 {
    let table = TaperHashMap::with_capacity(d.num_chunks);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let hv = TaperHashMap::bench_hash(d.hashes[i]);
        let pos = table.bench_chunk_pos(hv);
        let tag_hash = ((hv >> 16) & 0x7F) as u8;
        let tags = table.bench_chunk_at(pos).tags_u64();
        for slot in BitMask::match_tag(tags, tag_hash) {
            checksum = checksum.wrapping_add(slot as u64);
        }
    }
    checksum
}

#[inline(never)]
fn bench_compare_key_hash(d: &TestData) -> u64 {
    // Build a real hashmap
    let mut table = TaperHashMap::with_capacity(d.num_chunks);
    let build_hashes = &d.hashes[..d.num_keys];
    table.emplace_batch(build_hashes,
        |_, _| false,
        |_, _| {},
        |_, _, _| {},
    );
    // Probe: for each row, find chunk, compare key
    let mut match_count: u64 = 0;
    for i in 0..d.total_rows {
        let hv = TaperHashMap::bench_hash(d.hashes[i]);
        let pos = table.bench_chunk_pos(hv);
        let tag_hash = ((hv >> 16) & 0x7F) as u8;
        let chunk = table.bench_chunk_at(pos);
        let tags = chunk.tags_u64();
        for slot in BitMask::match_tag(tags, tag_hash) {
            let slot_idx = slot as usize;
            if chunk.keys[slot_idx] == hv {
                match_count += 1;
                break;
            }
        }
    }
    match_count
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
fn bench_serialize_key(d: &TestData) -> u64 {
    // SimpleArenaAllocator — exact replica of C++ taper::SimpleArenaAllocator (OmniOperator)
    // All 11 member variables preserved to match struct size and codegen behavior.
    struct SimpleArenaAllocator {
        min_chunk_size: i64,
        total_bytes: u64,
        used_bytes: u64,
        avail_bytes: u64,
        avail_buf: *mut u8,
        continuous_used_memory_bytes: u64,
        continuous_used: bool,
        growth_factor: u32,
        linear_growth_threshold: i64,
        chunks: Vec<*mut u8>,
        chunk_sizes: Vec<u64>,
    }
    impl SimpleArenaAllocator {
        fn new() -> Self {
            SimpleArenaAllocator {
                min_chunk_size: 4096,
                total_bytes: 0, used_bytes: 0, avail_bytes: 0,
                avail_buf: std::ptr::null_mut(),
                continuous_used_memory_bytes: 0, continuous_used: false,
                growth_factor: 2,
                linear_growth_threshold: 512 * 1024,
                chunks: Vec::new(), chunk_sizes: Vec::new(),
            }
        }
        fn get_next_size(&self, size: u64) -> u64 {
            if self.chunks.is_empty() {
                return size.max(self.min_chunk_size as u64);
            }
            let last = *self.chunk_sizes.last().unwrap();
            if last < self.linear_growth_threshold as u64 {
                size.max(last * self.growth_factor as u64)
            } else {
                let t = self.linear_growth_threshold as u64;
                ((size + t - 1) / t) * t
            }
        }
        fn allocate_chunk(&mut self, size: u64) {
            let ptr = unsafe { libc::malloc(size as usize) as *mut u8 };
            self.chunks.push(ptr);
            self.chunk_sizes.push(size);
            self.avail_buf = ptr;
            self.avail_bytes = size;
            self.total_bytes += size;
        }
        fn allocate(&mut self, size: usize) -> *mut u8 {
            if size == 0 {
                return std::ptr::NonNull::<u8>::dangling().as_ptr();
            }
            if self.avail_bytes < size as u64 {
                self.allocate_chunk(self.get_next_size(size as u64));
            }
            let ret = self.avail_buf;
            self.avail_buf = unsafe { self.avail_buf.add(size) };
            self.avail_bytes -= size as u64;
            ret
        }
    }
    impl Drop for SimpleArenaAllocator {
        fn drop(&mut self) {
            for &p in &self.chunks { unsafe { libc::free(p as *mut libc::c_void); } }
        }
    }

    let mut pool = SimpleArenaAllocator::new();
    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let mut total_size = 0usize;
        for c in 0..num_cols {
            total_size += 1 + compute_row_len_size(d.slices[c][i].len) as usize + d.slices[c][i].len;
        }
        let block = pool.allocate(total_size);
        let mut wp = block;
        for c in 0..num_cols {
            let written = serialize_varchar_to_buffer(wp, unsafe { std::slice::from_raw_parts(d.slices[c][i].ptr, d.slices[c][i].len) });
            wp = unsafe { wp.add(written) };
        }
        checksum = checksum.wrapping_add(block as u64);
    }
    checksum
}

/// 7a. Serialize with pre-allocated buffer (no allocator, no page faults)
#[inline(never)]
fn bench_serialize_prealloc(d: &TestData) -> u64 {
    let max_per_row = 4 * (1 + 4 + 30); // worst case per row
    let buf_size = d.total_rows * max_per_row;
    let buf = unsafe { libc::malloc(buf_size) as *mut u8 };
    unsafe { libc::memset(buf as *mut libc::c_void, 0, buf_size); } // touch all pages

    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut checksum: u64 = 0;
    let mut wp = buf;
    for i in 0..d.total_rows {
        let row_start = wp;
        for c in 0..num_cols {
            let written = serialize_varchar_to_buffer(wp, unsafe { std::slice::from_raw_parts(d.slices[c][i].ptr, d.slices[c][i].len) });
            wp = unsafe { wp.add(written) };
        }
        checksum = checksum.wrapping_add(row_start as u64);
    }
    unsafe { libc::free(buf as *mut libc::c_void); }
    checksum
}

/// 7b. Just memcpy the data bytes (no header, no rowLenSize)
#[inline(never)]
fn bench_memcpy_only(d: &TestData) -> u64 {
    let max_per_row = 4 * 30;
    let buf_size = d.total_rows * max_per_row;
    let buf = unsafe { libc::malloc(buf_size) as *mut u8 };
    unsafe { libc::memset(buf as *mut libc::c_void, 0, buf_size); }

    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut checksum: u64 = 0;
    let mut wp = buf;
    for i in 0..d.total_rows {
        for c in 0..num_cols {
            unsafe {
                libc::memcpy(wp as *mut libc::c_void, d.slices[c][i].ptr as *const libc::c_void, d.slices[c][i].len);
                wp = wp.add(d.slices[c][i].len);
            }
        }
        checksum = checksum.wrapping_add(wp as u64);
    }
    unsafe { libc::free(buf as *mut libc::c_void); }
    checksum
}

/// 7c. memcpy with flat array (eliminate vector-of-vector indirection)
#[inline(never)]
fn bench_memcpy_flat(d: &TestData) -> u64 {
    let max_per_row = 4 * 30;
    let buf_size = d.total_rows * max_per_row;
    let buf = unsafe { libc::malloc(buf_size) as *mut u8 };
    unsafe { libc::memset(buf as *mut libc::c_void, 0, buf_size); }

    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut checksum: u64 = 0;
    let mut wp = buf;
    let flat = d.flat_slices.as_ptr();
    for i in 0..d.total_rows {
        for c in 0..num_cols {
            unsafe {
                let s = &*flat.add(i * NUM_STR_COLS + c);
                libc::memcpy(wp as *mut libc::c_void, s.ptr as *const libc::c_void, s.len);
                wp = wp.add(s.len);
            }
        }
        checksum = checksum.wrapping_add(wp as u64);
    }
    unsafe { libc::free(buf as *mut libc::c_void); }
    checksum
}

/// Same as bench_serialize_key but uses noinline memcpy (forces PLT call like C++/GCC)
#[inline(never)]
fn bench_serialize_key_noinline(d: &TestData) -> u64 {
    struct SimpleArenaAllocator {
        avail_bytes: u64,
        avail_buf: *mut u8,
        chunks: Vec<(*mut u8, usize)>,
    }
    impl SimpleArenaAllocator {
        fn new() -> Self { SimpleArenaAllocator { avail_bytes: 0, avail_buf: std::ptr::null_mut(), chunks: Vec::new() } }
        fn allocate(&mut self, size: usize) -> *mut u8 {
            if self.avail_bytes < size as u64 {
                let chunk_size = size.max(if self.chunks.is_empty() { 4096 } else {
                    let last = self.chunks.last().unwrap().1;
                    if last < 512*1024 { last * 2 } else { ((size + 512*1024 - 1) / (512*1024)) * 512*1024 }
                });
                let ptr = unsafe { libc::malloc(chunk_size) as *mut u8 };
                self.chunks.push((ptr, chunk_size));
                self.avail_buf = ptr; self.avail_bytes = chunk_size as u64;
            }
            let ret = self.avail_buf;
            self.avail_buf = unsafe { self.avail_buf.add(size) };
            self.avail_bytes -= size as u64;
            ret
        }
    }
    impl Drop for SimpleArenaAllocator {
        fn drop(&mut self) { for &(p, _) in &self.chunks { unsafe { libc::free(p as *mut libc::c_void); } } }
    }

    let mut pool = SimpleArenaAllocator::new();
    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut checksum: u64 = 0;
    for i in 0..d.total_rows {
        let mut total_size = 0usize;
        for c in 0..num_cols {
            total_size += 1 + compute_row_len_size(d.slices[c][i].len) as usize + d.slices[c][i].len;
        }
        let block = pool.allocate(total_size);
        let mut wp = block;
        for c in 0..num_cols {
            let written = serialize_varchar_to_buffer_noinline_memcpy(wp, unsafe { std::slice::from_raw_parts(d.slices[c][i].ptr, d.slices[c][i].len) });
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
    // SimpleArenaAllocator — same as C++ taper::SimpleArenaAllocator
    struct SimpleArenaAllocator {
        chunks: Vec<(*mut u8, usize)>,
        buf: *mut u8,
        avail: usize,
    }
    impl SimpleArenaAllocator {
        fn new() -> Self { SimpleArenaAllocator { chunks: Vec::new(), buf: std::ptr::null_mut(), avail: 0 } }
        fn allocate(&mut self, size: usize) -> *mut u8 {
            if self.avail < size {
                let chunk_size = size.max(if self.chunks.is_empty() { 4096 } else {
                    let last = self.chunks.last().unwrap().1;
                    if last < 512*1024 { last * 2 } else { ((size + 512*1024 - 1) / (512*1024)) * 512*1024 }
                });
                let ptr = unsafe { libc::malloc(chunk_size) as *mut u8 };
                self.chunks.push((ptr, chunk_size));
                self.buf = ptr; self.avail = chunk_size;
            }
            let ret = self.buf;
            self.buf = unsafe { self.buf.add(size) };
            self.avail -= size;
            ret
        }
    }
    impl Drop for SimpleArenaAllocator {
        fn drop(&mut self) { for &(p, _) in &self.chunks { unsafe { libc::free(p as *mut libc::c_void); } } }
    }

    let mut pool = SimpleArenaAllocator::new();
    let num_cols = std::hint::black_box(NUM_STR_COLS);
    let mut blocks: Vec<*const u8> = Vec::with_capacity(d.total_rows);
    for i in 0..d.total_rows {
        let mut total_size = 0usize;
        for c in 0..num_cols {
            total_size += 1 + compute_row_len_size(d.slices[c][i].len) as usize + d.slices[c][i].len;
        }
        let block = pool.allocate(total_size);
        let mut wp = block;
        for c in 0..num_cols {
            let written = serialize_varchar_to_buffer(wp, unsafe { std::slice::from_raw_parts(d.slices[c][i].ptr, d.slices[c][i].len) });
            wp = unsafe { wp.add(written) };
        }
        blocks.push(block as *const u8);
    }
    let mut match_count: u64 = 0;
    for i in 0..d.total_rows {
        let mut pos = blocks[i];
        let mut ok = true;
        for c in 0..num_cols {
            let s = unsafe { std::slice::from_raw_parts(d.slices[c][i].ptr, d.slices[c][i].len) };
            if !compare_varchar_from_row(pos, s) { ok = false; break; }
            let entry_size = compute_varchar_serialized_size(pos);
            pos = unsafe { pos.add(entry_size) };
        }
        if ok { match_count += 1; }
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

// ─── Minimal isolated benchmarks (fixed data, no traversal) ─────────────

/// M1. serialize_single: one fixed 12-byte string, 4M iterations
#[inline(never)]
fn bench_serialize_single(d: &TestData) -> u64 {
    let iters = d.total_rows * NUM_STR_COLS;
    let test_str: &[u8] = b"key_12345_c0";
    let buf_size = iters * (1 + 1 + test_str.len());
    let buf = unsafe { libc::malloc(buf_size) as *mut u8 };
    unsafe { libc::memset(buf as *mut libc::c_void, 0, buf_size); }

    let mut wp = buf;
    for _ in 0..iters {
        let written = serialize_varchar_to_buffer(wp, test_str);
        wp = unsafe { wp.add(written) };
    }
    let checksum = wp as u64;
    unsafe { libc::free(buf as *mut libc::c_void); }
    checksum
}

/// M2. memcpy_single: just memcpy 12 bytes, 4M iterations
#[inline(never)]
fn bench_memcpy_single(d: &TestData) -> u64 {
    let iters = d.total_rows * NUM_STR_COLS;
    let test_str: &[u8] = b"key_12345_c0";
    let mut test_len = test_str.len();
    test_len = std::hint::black_box(test_len); // prevent constant propagation
    let buf_size = iters * test_len;
    let buf = unsafe { libc::malloc(buf_size) as *mut u8 };
    unsafe { libc::memset(buf as *mut libc::c_void, 0, buf_size); }

    let mut wp = buf;
    for _ in 0..iters {
        unsafe {
            libc::memcpy(wp as *mut libc::c_void, test_str.as_ptr() as *const libc::c_void, test_len);
            wp = wp.add(test_len);
        }
    }
    let checksum = wp as u64;
    unsafe { libc::free(buf as *mut libc::c_void); }
    checksum
}

/// M3. compare_single: CompareVarcharFromRow on one pre-serialized 12-byte string, 4M iters
#[inline(never)]
fn bench_compare_single(d: &TestData) -> u64 {
    let iters = d.total_rows * NUM_STR_COLS;
    let test_str: &[u8] = b"key_12345_c0";
    let mut serialized = [0u8; 20];
    serialize_varchar_to_buffer(serialized.as_mut_ptr(), test_str);

    let mut match_count: u64 = 0;
    for _ in 0..iters {
        if compare_varchar_from_row(serialized.as_ptr(), test_str) {
            match_count += 1;
        }
    }
    match_count
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

// ─── Main ────────────────────────────────────────────────────────
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let sel: f64 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(0.1);
    let num_iters: usize = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(DEFAULT_ITERS);
    let ht_size: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(16384);

    eprintln!("=== Rust EmplaceBatch Breakdown ===");
    eprintln!("sel={:.2}, iters={}, ht={}", sel, num_iters, ht_size);
    eprintln!("Generating data...");
    let data = gen_data(sel, ht_size);
    eprintln!("totalRows={}, numKeys={}, numChunks={}\n", data.total_rows, data.num_keys, data.num_chunks);

    let bench = |name: &str, f: fn(&TestData) -> u64| {
        let _ = f(&data);
        let t0 = Instant::now();
        let mut checksum: u64 = 0;
        for _ in 0..num_iters { checksum = f(&data); }
        let per_iter = t0.elapsed().as_secs_f64() * 1000.0 / num_iters as f64;
        println!("{:<30}  {:7.2} ms  checksum={}", name, per_iter, checksum);
    };

    // Optional stage filter: argv[4] = "5" or "7" or "9" etc.
    let stage_filter: String = args.get(4).cloned().unwrap_or_default();
    let should_run = |name: &str| -> bool {
        if stage_filter.is_empty() { return true; }
        stage_filter.contains(&name[..1]) || stage_filter.contains(name)
    };

    println!("=== Rust EmplaceBatch Breakdown (ht={}, sel={:.2}, {} iters, {} rows) ===",
             ht_size, sel, num_iters, data.total_rows);
    if should_run("1") { bench("1. hash_and_position", bench_hash_and_position); }
    if should_run("3") { bench("3. load_tags", bench_load_tags); }
    if should_run("4") { bench("4. match_tag_swar", bench_match_tag); }
    if should_run("5") { bench("5. compare_key_hash", bench_compare_key_hash); }
    if should_run("6") { bench("6. new_row", bench_new_row); }
    if should_run("7") { bench("7. serialize_key_4col", bench_serialize_key); }
    if should_run("7") { bench("7a.serialize_prealloc", bench_serialize_prealloc); }
    if should_run("7") { bench("7b.memcpy_only", bench_memcpy_only); }
    if should_run("7") { bench("7c.memcpy_flat", bench_memcpy_flat); }
    if should_run("7") { bench("7d.serialize_noinline_memcpy", bench_serialize_key_noinline); }
    if should_run("8") { bench("8. store_value_i64", bench_store_value); }
    if should_run("9") { bench("9. compare_varchar_4col", bench_compare_varchar); }
    if should_run("10") { bench("10. accumulate", bench_accumulate); }
    if should_run("F") || should_run("11") { bench("FULL: pipeline", bench_full_pipeline); }
    if should_run("M") { bench("M1.serialize_single_12B", bench_serialize_single); }
    if should_run("M") { bench("M2.memcpy_single_12B", bench_memcpy_single); }
    if should_run("M") { bench("M3.compare_single_12B", bench_compare_single); }
}
