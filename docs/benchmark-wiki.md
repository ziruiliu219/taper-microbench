# Taper Microbenchmark Wiki

## 1. 项目概述

对比 C++（GCC, 鲲鹏 aarch64）和 Rust（LLVM）实现的 TaperHashMap `EmplaceTableWithDecode` 全流程性能。
两边实现逻辑完全一致，对标 OmniOperator 的 `TaperColumnSerializeHandler`。

---

## 2. 代码结构

```
taper-microbench/
├── cpp/
│   ├── include/
│   │   ├── taper_hashtable.h          # TaperFlatHashTable（hash table 核心）
│   │   ├── column_marshaller.h        # TaperColumnSerializeHandler（5步流程）
│   │   ├── row_container.h            # RowContainer（行存储）
│   │   └── simple_arena_allocator.h   # SimpleArenaAllocator（内存分配）
│   └── src/
│       ├── emplace_breakdown_bench.cpp # ★ 主 benchmark（按 step 拆解）
│       ├── step_by_step_bench.cpp      # 简化版 step benchmark
│       ├── micro_pipeline_bench.cpp    # 增量叠加 benchmark
│       └── profile_taper_standalone.cpp
├── rust/
│   └── src/
│       ├── taper_hashmap.rs           # TaperHashMap
│       ├── column_marshaller.rs       # TaperColumnSerializeHandler
│       ├── row_container.rs           # RowContainer
│       ├── batch_compare.rs           # BatchCompare（SIMD/scalar）
│       ├── bitmask.rs                 # BitMask（SWAR）
│       ├── chunk.rs                   # Chunk + SlotValue
│       └── bin/
│           ├── emplace_breakdown_bench.rs  # ★ 主 benchmark
│           ├── step_by_step_bench.rs
│           └── micro_pipeline_bench.rs
├── run_breakdown_matrix.sh            # 跑全 matrix 脚本
├── run_all.sh                         # 简单全量输出
└── docs/
    └── benchmark-wiki.md              # 本文件
```

---

## 3. 核心数据结构

```
┌─────────────────────────────────────────────────────────────┐
│                    TaperFlatHashTable                         │
│                                                              │
│  chunks: *mut Chunk  (posix_memalign 128B 对齐)              │
│  num_chunks: 2的幂                                           │
│  mask: num_chunks - 1                                        │
│  size: 当前已插入 entry 数                                    │
│                                                              │
│  ┌─── Chunk (128 bytes = 2 cache lines) ───┐                │
│  │ tags:   [u8; 8]     @ offset 0          │                │
│  │ keys:   [u64; 8]    @ offset 8          │                │
│  │ _pad:   [u8; 8]     @ offset 72         │                │
│  │ values: [SlotValue; 8] @ offset 80      │                │
│  └──────────────────────────────────────────┘                │
│                                                              │
│  SlotValue = 6 bytes (compressed 48-bit row pointer)         │
└─────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────┐
│                    RowContainer                               │
│                                                              │
│  Row layout: [col0][col1]...[colN][null_bytes][agg_state]   │
│  - Fixed col: 直接存 value（如 i64 = 8 bytes）              │
│  - Varchar col: 存 8-byte pointer → arena 数据               │
│                                                              │
│  Arena 数据格式: [rowLenSize:1B][length:1-4B][data:NB]       │
│  - rowLenSize=1: length ≤ 255                                │
│  - rowLenSize=2: length ≤ 65535                              │
│  - rowLenSize=4: length ≤ 4GB                                │
└─────────────────────────────────────────────────────────────┘
```

---

## 4. EmplaceTableWithDecode 5步流程

```
┌─────────────────────────────────────────────────────────────────────┐
│            EmplaceTableWithDecode（per batch, 410 rows）              │
│                                                                     │
│  Step 1: Hash                                                       │
│  ┌─────────────────┐                                                │
│  │ hash[i] = key   │  (KeyScattered = identity hash)                │
│  │ pos[i] = hash & mask                                             │
│  └────────┬────────┘                                                │
│           ▼                                                         │
│  Step 2+3: EmplaceBatch (hash table probe + insert)                 │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ for each row:                                                │    │
│  │   prefetch chunk[pos[i+16]]                                  │    │
│  │   TryEmplaceAtPos:                                           │    │
│  │     tag_match → keys[slot] == hash → on_update(existing)     │    │
│  │     empty_slot → on_init(new group):                         │    │
│  │       ├─ NewRow()                                            │    │
│  │       ├─ StoreKeyOneRow (serialize 4 varchar → arena) ←Step7│    │
│  │       ├─ StoreValue<i64>(agg)                        ←Step8 │    │
│  │       └─ set SlotValue = row pointer                         │    │
│  │     chunk_full → linear probe next chunk                     │    │
│  │   collect collisions → iterate until resolved                │    │
│  └────────┬────────────────────────────────────────────────────┘    │
│           ▼                                                         │
│  Step 4: GetUnequalsNumWithDecode (batch key comparison)            │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ for each update_row:                                         │    │
│  │   read arena_ptr from group row                              │    │
│  │   for each varchar col:                                      │    │
│  │     CompareVarcharFromRow(arena, input) ← memcmp     ←Step9 │    │
│  │   if unequal → move to front of unequals list                │    │
│  └────────┬────────────────────────────────────────────────────┘    │
│           ▼                                                         │
│  Step 5: Re-emplace unequals (scalar per-row Emplace)               │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │ for each unequal row:                                        │    │
│  │   Emplace(hash, full_key_cmp, on_init, on_update)            │    │
│  └────────┬────────────────────────────────────────────────────┘    │
│           ▼                                                         │
│  Accumulate: agg += value for confirmed-equal rows          ←Step10 │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 5. Benchmark Step 对应关系

```
┌───────────────────────────────────────────────────────────────────┐
│  Step 名称              │ 测什么                  │ 调用方式        │
├─────────────────────────┼─────────────────────────┼─────────────────┤
│ 1. hash_and_position    │ Hash + ChunkPos         │ 独立循环        │
│ 3. load_tags            │ chunk->TagsU64()        │ 独立循环        │
│ 4. match_tag_swar       │ PHBitMask::MatchTag     │ 独立循环        │
│ 5. compare_key_hash     │ keys[slot]==hash (i64)  │ 预建 HT 后 probe│
│ 6. new_row              │ RowContainer::NewRow    │ 独立循环        │
│ 7. serialize_key_4col   │ SerializeVarcharToBuffer│ emplace 回调    │
│ 8. store_value_i64      │ StoreValue<i64>         │ 独立循环        │
│ 9. compare_varchar_4col │ CompareVarcharFromRow   │ emplace+compare │
│ 10. accumulate          │ agg += value            │ 独立循环        │
│ FULL: pipeline          │ EmplaceTableWithDecode  │ batch 驱动      │
└───────────────────────────────────────────────────────────────────┘
```

---

## 6. 数据生成与防 Rehash

```
┌─────────────────────────────────────────────────────────────────┐
│  输入: ht_size (总 slot 数), sel (build/probe 分配比例)           │
│                                                                 │
│  numChunks = (ht_size / 8).next_power_of_two()                  │
│  capacity = numChunks × 8                                       │
│                                                                 │
│  ┌─────────────────────────────────────────────┐                │
│  │ distinctKeys = capacity × 9 / 10 - 1        │ ← 整数运算     │
│  │ expandThreshold = capacity × 9 / 10         │                │
│  │                                             │                │
│  │ distinctKeys < expandThreshold              │ ← 永不 expand  │
│  └─────────────────────────────────────────────┘                │
│                                                                 │
│  numKeys = distinctKeys × sel      (build 阶段插入)              │
│  probeMisses = distinctKeys - numKeys (probe 阶段新 key)         │
│  probeHits = 1M - probeMisses      (probe 命中已有 key)          │
│                                                                 │
│  Hash Table 填充过程:                                            │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ size                                                     │    │
│  │  ▲                                                       │    │
│  │  │ expandThreshold ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─    │    │
│  │  │ distinctKeys ─────────────────────────────●           │    │
│  │  │                                     ╱                 │    │
│  │  │ numKeys ──────────●──────────────╱                    │    │
│  │  │              ╱                                        │    │
│  │  │───────────╱─────────────────────────────▶ time        │    │
│  │  │  build 阶段  │     probe 阶段                         │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

---

## 7. 关键函数对比（C++ vs Rust）

### SerializeVarcharToBuffer

```
C++:                                    Rust:
─────────────────────────────────────   ─────────────────────────────────────
*writePos = rowLenSize;                 *write_pos = row_len_size;
memcpy(writePos+1, &l32, rowLenSize);   libc::memcpy(+1, &l32, rls);
memcpy(writePos+1+rls, data, len);      libc::memcpy(+1+rls, data, len);
return 1 + rls + len;                   return 1 + rls + len;
```

### CompareVarcharFromRow

```
C++:                                    Rust:
─────────────────────────────────────   ─────────────────────────────────────
rowLenSize = *rowData;                  row_len_size = *arena_ptr;
switch(rls) → parse stringLen           match rls → parse string_len
if (stringLen != inputLen) return false  if string_len != input.len() → false
memcmp(stored, input, len) == 0         memcmp_ptr(stored, input, len) == 0
                                        ↑ 函数指针调用（阻止 bcmp 优化）
```

### TryEmplaceAtPos

```
C++ / Rust 完全一致:
─────────────────────────────────────
1. tag_hash = (hash >> 16) & 0x7F
2. tags = chunk.TagsU64()
3. MatchTag(tags, tag_hash) → iterate:
     if keys[slot] == hash → on_update(existing)
4. MatchEmpty(tags) → first empty:
     size++; tags[slot] = tag_hash
     keys[slot] = hash
     on_init(new) → on_update(new)
5. return false (chunk full → linear probe)
```

### BitMask / PHBitMask (SWAR)

```
两边完全一致:
─────────────────────────────────────
MatchTag:  (x - LSBS) & ~x & MSBS    where x = tags ^ (LSBS * target)
MatchEmpty: (tags & (~tags << 7)) & MSBS
Iterator:   slot = ctz(mask) >> 3;  mask &= (mask - 1)
```

---

## 8. Benchmark 设计决策

### 为什么 step 7/9 用 emplace 回调驱动？

早期版本用独立循环遍历 `d.slices[c][i]`（二维 vector），GCC 生成了低效代码：

```
早期（独立循环）:              现在（emplace 回调）:
─────────────────────────────  ─────────────────────────────
for i in 0..N:                 table.EmplaceBatch(hashes,
  for c in 0..4:                 on_init = |rowIdx| {
    serialize(slices[c][i])        for c in 0..4:
                                     serialize(colSlices[c][rowIdx])
                                 })

GCC 每次循环 reload base ptr    GCC 在 lambda inline 后
(3层指针追逐 = 慢2x)           指针已在寄存器中 (一致)
```

### 为什么 Rust compare 用函数指针调 memcmp？

LLVM 自动优化 `memcmp(...)==0` → `bcmp`（只判等，更快退出）。
C++/GCC 不做这个优化。为了公平对比，Rust 用 `black_box(libc::memcmp)` 函数指针阻止此优化。

### 为什么 accumulate 用 branch 替代 modulo？

```
早期: rows[i % numKeys]    →  GCC 每次生成 udiv 指令 (慢)
现在: if (++idx >= n) idx=0 →  两边都是 branch (一致)
```

---

## 9. 用法

```bash
# 编译
cd cpp/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j
cd ../../rust && cargo build --release

# 单组对比
taskset -c 0 ./cpp/build/cpp_emplace_breakdown 0.5 5 262144
taskset -c 0 ./rust/target/release/emplace_breakdown_bench 0.5 5 262144

# 参数说明: <sel> [iters] [ht_size] [stage_filter]
# stage_filter: "7" 只跑 serialize, "M" 只跑 minimal, 空=全部

# 全 matrix (4 ht × 5 sel × C++/Rust)
./run_breakdown_matrix.sh 5
```

---

## 10. 结论

| 维度 | 结果 |
|------|------|
| FULL pipeline | C++ ≈ Rust（ratio 1.02x 平均） |
| 各 step 独立 | 对齐后所有 step ratio 接近 1:1 |
| 核心函数（单次调用） | M1/M2 验证完全一致 |
| 根因 | 早期差异来自 benchmark harness，不是实现差异 |

两边代码逻辑完全一致，最终性能差异 < 5%，在测量噪声范围内。
