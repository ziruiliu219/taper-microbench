# Taper Microbenchmark Wiki

## 概述

本项目对比 C++（GCC, 鲲鹏 aarch64）和 Rust（LLVM）实现的 TaperHashMap EmplaceBatch 全流程性能。
核心 benchmark 是 `emplace_breakdown_bench`，将 `EmplaceTableWithDecode` 的完整流程拆解为独立可测的子步骤。

## 参数

| 参数 | 含义 | 默认值 |
|------|------|--------|
| `sel` | build/probe 分配比例。`sel=0.5` 表示 50% distinct key 在 build 阶段插入，50% 在 probe 阶段作为 miss 插入 | 0.1 |
| `ht_size` | hash table 总 slot 数（= numChunks × 8） | 16384 |
| `iters` | 每个 step 重复执行次数（取平均） | 5 |

### 防 rehash 保证

```
distinctKeys = capacity * 9 / 10 - 1
expandThreshold = capacity * 9 / 10

distinctKeys < expandThreshold → 永远不会触发 ShouldExpand()
```

用整数运算，零浮点依赖，数学上 100% 保证不 rehash。

## 用法

```bash
# 编译
cd cpp/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j
cd ../../rust && cargo build --release

# 单次运行（所有 step）
./cpp/build/cpp_emplace_breakdown 0.5 5 262144
./rust/target/release/emplace_breakdown_bench 0.5 5 262144

# 只跑特定 step（用 stage filter）
./cpp/build/cpp_emplace_breakdown 0.5 5 262144 7     # 只跑 serialize
./cpp/build/cpp_emplace_breakdown 0.5 5 262144 M     # 只跑 minimal benchmarks

# 跑全 matrix（4 ht × 5 sel × C++/Rust）
./run_breakdown_matrix.sh 5
```

## Step 拆解

### 主要 Step（对应 FULL pipeline 的各阶段）

| Step | 名称 | 测试内容 | 调用方式 |
|------|------|----------|----------|
| 1 | hash_and_position | `Hash(key)` + `GetChunkPos(hash)` | 独立循环 |
| 3 | load_tags | `chunk->TagsU64()` | 独立循环 |
| 4 | match_tag_swar | `PHBitMask::MatchTag` (SWAR 位匹配) | 独立循环 |
| 5 | compare_key_hash | tag match 后 `keys[slot] == hash` (int64 比较) | 在已建好的 hashmap 上 probe |
| 6 | new_row | `RowContainer::NewRow()` (arena bump 分配) | 独立循环 |
| **7** | **serialize_key_4col** | **StoreKeyOneRow: 4 列 varchar serialize 到 arena** | **emplace 回调驱动** |
| 8 | store_value_i64 | `StoreValue<i64>` (写 agg state) | 独立循环 |
| **9** | **compare_varchar_4col** | **CompareVarcharFromRow × 4 列** | **emplace 建组 + batch compare** |
| 10 | accumulate | `agg += value` (原地累加) | 独立循环 |
| FULL | pipeline | 完整 `EmplaceTableWithDecode`（5 步流程） | batch 驱动 |

### Step 7 (serialize) 详细说明

**调用方式**：通过 `EmplaceBatch` / `emplace_batch_full` 的 `on_init` 回调驱动（不是独立循环），与 FULL pipeline 完全相同的执行路径。

**测量内容**：
1. 从 `colSlices[c][rowIdx]` 读取 varchar ptr/len
2. 计算 totalSize（4 列）
3. `ArenaAlloc(totalSize)`
4. `SerializeVarcharToBuffer` × 4 列
5. 写 row pointer 到 hash table slot

**关键设计**：早期版本用独立循环遍历 `d.slices[c][i]`（二维 vector），导致 GCC 生成低效代码（多一层指针追逐），C++ 比 Rust 慢 2x。改为 emplace 回调驱动后，两边代码在相同的 inline context 下执行，性能一致（ratio ≈ 1:1）。

### Step 9 (compare_vc) 详细说明

**调用方式**：分两阶段——
1. **Setup**：通过 emplace 建好所有 groups（serialize 到 RowContainer）
2. **Compare**：batch-driven 遍历所有 row，从 group 读 arena 指针，对 4 列分别调 `CompareVarcharFromRow`

**测量内容**（包含 setup + compare）：
- Setup：emplace + serialize（同 step 7）
- Compare：从 row 读 pointer → parse arena format → `memcmp` 对比

**注意**：step 9 的绝对值 > step 7，因为它包含了完整的 emplace 建组 + compare 两部分。

### Step 10 (accumulate) 详细说明

循环遍历所有行，对目标 group 做 `agg += value`。用 `if (++idx >= numKeys) idx = 0` 替代 `i % numKeys` 取模，避免 GCC 未优化的 `udiv` 指令导致不公平差异。

## Minimal Benchmarks (M1/M2/M3)

用于定位具体函数的单次调用开销，排除数据遍历影响：

| Step | 内容 | 迭代次数 |
|------|------|----------|
| M1 | `SerializeVarcharToBuffer` 对固定 12 字节字符串 | totalRows × 4 |
| M2 | `memcpy` 12 字节（运行时长度，阻止常量传播） | totalRows × 4 |
| M3 | `CompareVarcharFromRow` 对固定 12 字节 | totalRows × 4 |

这些验证了：**单次函数调用 C++ 和 Rust 性能一致**（ratio ≈ 1:1），差异只来自调用上下文（循环结构、数据访问模式）。

## 辅助 Step（7a/7b/7c/7e）

用于诊断 serialize 差异来源的实验性 step：

| Step | 内容 | 发现 |
|------|------|------|
| 7a | serialize 用预分配 buffer（无 allocator） | 排除了 allocator 是瓶颈 |
| 7b | 只做 memcpy 数据（遍历 `slices[c][i]`） | 发现遍历模式是瓶颈 |
| 7c | memcpy 用 flat 数组（无 vector-of-vector） | 证明 vector indirection 有部分影响 |
| 7e | 强制 load-before-memcpy 串行化 | 排除了 OOO 执行差异 |

## 核心发现

1. **FULL pipeline C++ ≈ Rust**（ratio 1.02x 平均）
2. **独立 step 的差异来源**：
   - serialize/compare_vc 在独立循环中 C++ 慢 2x → 改为 emplace 驱动后对齐
   - 根因：GCC 对 `vector<vector<T>>[c][i]` 遍历不做 loop-invariant code motion（每次循环从内存重新 load base pointer），LLVM 做了
3. **memcmp vs bcmp**：LLVM 自动把 `memcmp(...)==0` 优化为 `bcmp`（只判等，更快）。Rust 已强制使用 `memcmp` 函数指针来对齐 C++ 行为
4. **取模开销**：GCC 不优化循环中的 `i % n`，LLVM 做 strength reduction。已改用 branch 替代

## 数据生成

```
numChunks = ht_size / 8 (向上取 2 的幂)
capacity = numChunks × 8
distinctKeys = capacity * 9 / 10 - 1

numKeys = distinctKeys × sel        (build 阶段插入)
probeMisses = distinctKeys - numKeys (probe 阶段新 key)
probeHits = NUM_PROBE_ROWS - probeMisses (probe 命中已有 key)
```

所有数据包含 4 列 varchar，字符串格式 `key_{id}_c{col}`（约 8-15 字节）。

## 文件结构

```
cpp/include/taper_hashtable.h     — TaperFlatHashTable 实现（同 OmniOperator）
cpp/include/column_marshaller.h   — TaperColumnSerializeHandler（同 OmniOperator）
cpp/include/row_container.h       — RowContainer（同 OmniOperator）
cpp/src/emplace_breakdown_bench.cpp — C++ benchmark

rust/src/taper_hashmap.rs         — TaperHashMap 实现
rust/src/column_marshaller.rs     — TaperColumnSerializeHandler
rust/src/row_container.rs         — RowContainer
rust/src/bin/emplace_breakdown_bench.rs — Rust benchmark

run_breakdown_matrix.sh           — 跑全 matrix 脚本
```
