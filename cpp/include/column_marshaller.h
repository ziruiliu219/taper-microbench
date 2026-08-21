/**
 * TaperColumnSerializeHandler — optimized to match Rust's access patterns exactly.
 * Pre-computed column offsets (no per-row ColumnAt calls), direct RowContainer access.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
#include <algorithm>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#include "taper_hashtable.h"
#include "row_container.h"
#include "simple_arena_allocator.h"

namespace taper {

enum class ColumnDesc { Int64, Varchar };

struct VarcharSlice {
    const uint8_t* ptr;
    size_t len;
};

struct ColumnInput {
    ColumnDesc type;
    union { const int64_t* int64Data; const VarcharSlice* vcSlices; };
    static ColumnInput MakeInt64(const int64_t* d) { ColumnInput c; c.type=ColumnDesc::Int64; c.int64Data=d; return c; }
    static ColumnInput MakeVarchar(const VarcharSlice* s) { ColumnInput c; c.type=ColumnDesc::Varchar; c.vcSlices=s; return c; }
};

// ─── Varchar helpers ─────────────────────────────────────────────────────────

// Force-inline macro: GCC/Clang use __attribute__((always_inline)), MSVC uses __forceinline.
#if defined(__GNUC__) || defined(__clang__)
#define TAPER_FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define TAPER_FORCE_INLINE __forceinline
#else
#define TAPER_FORCE_INLINE inline
#endif

/// Inline byte-sequence equality for short strings (0–32 bytes).
/// Uses fixed-width memcpy (compiled to unaligned load instructions, no function call)
/// to avoid the overhead of calling libc memcmp/bcmp for short buffers.
///
/// For 8–16 bytes: compare first 8 and last 8 bytes (overlap is harmless).
/// For 4–7 bytes: compare first 4 and last 4 bytes.
/// For 1–3 bytes: compare first, middle, and last byte.
/// For 17–32 bytes: compare first 16 and last 16 bytes.
/// For 0 bytes: always equal.
/// For >32 bytes: falls back to memcmp.
///
/// All reads use memcpy to avoid alignment and strict-aliasing issues.
/// Safe on x86-64, AArch64, and Apple Silicon.
static TAPER_FORCE_INLINE bool InlineMemEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    if (len == 0) return true;

    if (len >= 8 && len <= 16) {
        uint64_t a0, b0, a1, b1;
        memcpy(&a0, a, 8);
        memcpy(&b0, b, 8);
        memcpy(&a1, a + len - 8, 8);
        memcpy(&b1, b + len - 8, 8);
        return (a0 == b0) & (a1 == b1);
    }

    if (len >= 4 && len < 8) {
        uint32_t a0, b0, a1, b1;
        memcpy(&a0, a, 4);
        memcpy(&b0, b, 4);
        memcpy(&a1, a + len - 4, 4);
        memcpy(&b1, b + len - 4, 4);
        return (a0 == b0) & (a1 == b1);
    }

    if (len < 4) {
        // 1–3 bytes: compare first, middle, last
        // For len=1: first==last, mid index is 0 (harmless repeat)
        // For len=2: first, mid=len/2=1, last=1 (covers both bytes)
        // For len=3: first=0, mid=1, last=2
        return (a[0] == b[0]) & (a[len >> 1] == b[len >> 1]) & (a[len - 1] == b[len - 1]);
    }

    if (len <= 32) {
        // 17–32 bytes: compare first 16 and last 16
        uint64_t a0, b0, a1, b1, a2, b2, a3, b3;
        memcpy(&a0, a, 8);
        memcpy(&b0, b, 8);
        memcpy(&a1, a + 8, 8);
        memcpy(&b1, b + 8, 8);
        memcpy(&a2, a + len - 16, 8);
        memcpy(&b2, b + len - 16, 8);
        memcpy(&a3, a + len - 8, 8);
        memcpy(&b3, b + len - 8, 8);
        return (a0 == b0) & (a1 == b1) & (a2 == b2) & (a3 == b3);
    }

    // >32 bytes: fall back to memcmp
    return memcmp(a, b, len) == 0;
}

inline uint8_t ComputeRowLenSize(size_t len) { return len<=0xFF?1:len<=0xFFFF?2:4; }

inline size_t SerializeVarcharToBuffer(uint8_t* writePos, const uint8_t* data, size_t len) {
    uint8_t rowLenSize = ComputeRowLenSize(len); *writePos = rowLenSize;
    uint32_t l32 = static_cast<uint32_t>(len); memcpy(writePos+1, &l32, rowLenSize);
    if (len) memcpy(writePos+1+rowLenSize, data, len);
    return 1+rowLenSize+len;
}

inline size_t ComputeVarCharSerializedSize(const uint8_t* data) {
    uint8_t rowLenSize = *data; if (!rowLenSize) return 1;
    size_t stringLen=0;
    switch(rowLenSize){case 1:stringLen=*(data+1);break;case 2:{uint16_t v;memcpy(&v,data+1,2);stringLen=v;break;}default:{uint32_t v;memcpy(&v,data+1,4);stringLen=v;}}
    return 1+rowLenSize+stringLen;
}

inline bool CompareVarcharFromRow(const uint8_t* rowData, const uint8_t* input, size_t inputLen) {
    uint8_t rowLenSize = *rowData; if(!rowLenSize) return false;
    size_t stringLen=0;
    switch(rowLenSize){case 1:stringLen=*(rowData+1);break;case 2:{uint16_t v;memcpy(&v,rowData+1,2);stringLen=v;break;}default:{uint32_t v;memcpy(&v,rowData+1,4);stringLen=v;}}
    if (stringLen!=inputLen) return false;
    if (stringLen==0) return true;
    const uint8_t* stored = rowData+1+rowLenSize;
    return memcmp(stored, input, stringLen) == 0;
}

// ─── SetRowPtr / GetRowPtr ────────────────────────────────────────────────────

static inline void SetRowPtr(char* buf, uint8_t* ptr) {
    uint64_t val = reinterpret_cast<uint64_t>(ptr);
    // Store lower 6 bytes directly (avoids libc memcpy call for non-power-of-2 size)
    uint32_t lo; memcpy(&lo, &val, 4);
    uint16_t hi; memcpy(&hi, reinterpret_cast<const char*>(&val) + 4, 2);
    memcpy(buf, &lo, 4);
    memcpy(buf + 4, &hi, 2);
}

static inline uint8_t* GetRowPtr(const char* buf) {
    uint64_t val = 0;
    uint32_t lo; memcpy(&lo, buf, 4);
    uint16_t hi; memcpy(&hi, buf + 4, 2);
    val = static_cast<uint64_t>(hi) << 32 | lo;
    return reinterpret_cast<uint8_t*>(val);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TaperColumnSerializeHandler
// ═══════════════════════════════════════════════════════════════════════════════

class TaperColumnSerializeHandler {
public:
    using HashTable = TaperFlatHashTable;

private:
    HashTable table_;
    RowContainer aggRows_;
    std::vector<ColumnDesc> colDescs_;
    int32_t groupColNum_ = 0;
    int32_t aggOffset_ = 0;

    // Pre-computed column layout (matches Rust col_offsets, varchar_col_indices, etc.)
    std::vector<int32_t> colOffsets_;       // colOffsets_[i] = RowColumn.Offset() for col i
    std::vector<uint32_t> colNullBytes_;    // null byte offset for col i
    std::vector<uint8_t> colNullMasks_;     // null mask for col i
    std::vector<int32_t> varcharColIndices_;
    int32_t varcharSlotColIdx_ = -1;
    int32_t varcharSlotOffset_ = 0;
    bool useMerged_ = false;
    std::vector<int32_t> colToVarcharPos_;

    // Reusable buffers
    std::vector<int32_t> workingUpdateIndices_;
    int32_t workingUpdateCount_ = 0;
    std::vector<const uint8_t*> mergedVarcharCache_;
    int32_t mergedVarcharCacheCount_ = 0;
    std::vector<uint8_t*> groups_;

public:
    TaperColumnSerializeHandler(SimpleArenaAllocator& pool, int32_t aggStatesSize,
                                const std::vector<ColumnDesc>& colDescs, size_t initCap)
        : table_(initCap), aggRows_(BuildRowContainer(colDescs, aggStatesSize, pool)),
          colDescs_(colDescs), groupColNum_(static_cast<int32_t>(colDescs.size()))
    {
        aggOffset_ = aggRows_.AggStateOffset();

        // Pre-compute column offsets and null info
        colOffsets_.resize(groupColNum_);
        colNullBytes_.resize(groupColNum_);
        colNullMasks_.resize(groupColNum_);
        for (int32_t i = 0; i < groupColNum_; i++) {
            auto col = aggRows_.ColumnAt(i);
            colOffsets_[i] = col.Offset();
            colNullBytes_[i] = col.NullByte();
            colNullMasks_[i] = col.NullMask();
        }

        // Varchar index setup
        for (int32_t i = 0; i < groupColNum_; i++) {
            if (colDescs[i] == ColumnDesc::Varchar)
                varcharColIndices_.push_back(i);
        }
        useMerged_ = varcharColIndices_.size() > 1;
        if (!varcharColIndices_.empty()) {
            varcharSlotColIdx_ = varcharColIndices_[0];
            varcharSlotOffset_ = colOffsets_[varcharSlotColIdx_];
        }
        colToVarcharPos_.assign(groupColNum_, -1);
        for (int32_t v = 0; v < static_cast<int32_t>(varcharColIndices_.size()); ++v)
            colToVarcharPos_[varcharColIndices_[v]] = v;
    }

    int32_t AggStateOffset() const { return aggOffset_; }
    size_t NumGroups() const { return aggRows_.NumRows(); }

    /// Sum all agg i64 values across all groups (for verification).
    int64_t AggChecksum() const {
        int64_t sum = 0;
        // Iterate hash table: every occupied slot has a row pointer
        size_t numChunks = static_cast<size_t>(table_.LastChunkIdx()) + 1;
        for (size_t ci = 0; ci < numChunks; ci++) {
            const auto* chunk = table_.ChunkAt(ci);
            for (uint32_t s = 0; s < kSlotsPerChunk; s++) {
                if (chunk->tags[s] != kEmptyTag) {
                    uint64_t val = 0;
                    memcpy(&val, chunk->values[s].bytes, ROW_PTR_SIZE);
                    const char* row = reinterpret_cast<const char*>(val);
                    int64_t aggVal;
                    memcpy(&aggVal, row + aggOffset_, sizeof(aggVal));
                    sum += aggVal;
                }
            }
        }
        return sum;
    }

    void EmplaceTableWithDecode(const int64_t* hashes, int32_t rowsNum,
        const std::vector<ColumnInput>& columns, const int64_t* aggValues)
    {
        if (rowsNum <= 0) return;

        groups_.resize(rowsNum);
        workingUpdateIndices_.resize(rowsNum);
        workingUpdateCount_ = 0;

        RowContainer* rc = &aggRows_;
        const int32_t aggOffset = aggOffset_;
        const ColumnInput* colsPtr = columns.data();

        // Step 2: EmplaceBatch — init stores keys immediately (matches Rust on_init)
        table_.EmplaceBatch(hashes, rowsNum,
            [](int32_t) { return false; },
            [&](uint32_t rowIdx, char* data) {
                char* row = rc->NewRow();
                SetRowPtr(data, reinterpret_cast<uint8_t*>(row));
                StoreKeyOneRow(row, rowIdx, colsPtr);
                RowContainer::StoreValue<int64_t>(row, aggOffset, aggValues[rowIdx]);
            },
            [&](uint32_t rowIdx, char* data, bool initFlag) {
                groups_[rowIdx] = GetRowPtr(data);
                if (!initFlag) {
                    workingUpdateIndices_[workingUpdateCount_++] = rowIdx;
                }
            }
        );

        if (workingUpdateCount_ == 0) return;

        // Step 4: GetUnequalsNumWithDecode
        int32_t count = workingUpdateCount_;
        int32_t unequalsNum = GetUnequalsNumWithDecode(count, columns);

        // Step 5: re-emplace collisions
        for (int32_t ui = 0; ui < unequalsNum; ui++) {
            int32_t rowIdx = workingUpdateIndices_[ui];
            int64_t hash = hashes[rowIdx];
            table_.Emplace(hash,
                [&](const char* valBuf) -> bool {
                    return CompareKeysWithDecode(GetRowPtr(valBuf), rowIdx, columns);
                },
                [&](char* data) {
                    char* row = aggRows_.NewRow();
                    SetRowPtr(data, reinterpret_cast<uint8_t*>(row));
                    StoreKeyOneRow(row, rowIdx, colsPtr);
                    RowContainer::StoreValue<int64_t>(row, aggOffset, aggValues[rowIdx]);
                },
                [&](char* data, bool isNew) {
                    if (!isNew) {
                        auto* rp = GetRowPtr(data);
                        *reinterpret_cast<int64_t*>(rp + aggOffset) += aggValues[rowIdx];
                    }
                    groups_[rowIdx] = GetRowPtr(data);
                }
            );
        }

        // Accumulate agg for confirmed-equal rows
        for (int32_t ui = unequalsNum; ui < count; ui++) {
            int32_t rowIdx = workingUpdateIndices_[ui];
            auto* rp = groups_[rowIdx];
            *reinterpret_cast<int64_t*>(rp + aggOffset_) += aggValues[rowIdx];
        }
    }

private:
    static RowContainer BuildRowContainer(const std::vector<ColumnDesc>& colDescs, int32_t aggStatesSize, SimpleArenaAllocator& pool) {
        std::vector<size_t> keySizes;
        std::vector<ColumnKind> kinds;
        for (auto& cd : colDescs) {
            if (cd == ColumnDesc::Int64) { keySizes.push_back(8); kinds.push_back(ColumnKind::Fixed); }
            else { keySizes.push_back(0); kinds.push_back(ColumnKind::Varchar); }
        }
        return RowContainer(keySizes, kinds, aggStatesSize, pool);
    }

    // ─── Hot path: store keys for one row ───

    TAPER_FORCE_INLINE void StoreKeyOneRow(char* row, int32_t rowIdx, const ColumnInput* cols) {
        if (useMerged_) {
            size_t totalSize = 0;
            for (int32_t v = 0; v < static_cast<int32_t>(varcharColIndices_.size()); v++) {
                int32_t ci = varcharColIndices_[v];
                totalSize += 1 + ComputeRowLenSize(cols[ci].vcSlices[rowIdx].len) + cols[ci].vcSlices[rowIdx].len;
            }
            uint8_t* blockStart = aggRows_.ArenaAlloc(totalSize);
            uint8_t* wp = blockStart;
            for (int32_t v = 0; v < static_cast<int32_t>(varcharColIndices_.size()); v++) {
                int32_t ci = varcharColIndices_[v];
                RowContainer::ClearNullAt(row, colNullBytes_[ci], colNullMasks_[ci]);
                wp += SerializeVarcharToBuffer(wp, cols[ci].vcSlices[rowIdx].ptr, cols[ci].vcSlices[rowIdx].len);
            }
            memcpy(row + varcharSlotOffset_, &blockStart, sizeof(blockStart));
        } else if (!varcharColIndices_.empty()) {
            int32_t ci = varcharColIndices_[0];
            RowContainer::ClearNullAt(row, colNullBytes_[ci], colNullMasks_[ci]);
            size_t len = cols[ci].vcSlices[rowIdx].len;
            uint8_t* ap = aggRows_.ArenaAlloc(1 + ComputeRowLenSize(len) + len);
            SerializeVarcharToBuffer(ap, cols[ci].vcSlices[rowIdx].ptr, len);
            memcpy(row + colOffsets_[ci], &ap, sizeof(ap));
        }
        for (int32_t ci = 0; ci < groupColNum_; ci++) {
            if (colDescs_[ci] == ColumnDesc::Int64) {
                RowContainer::ClearNullAt(row, colNullBytes_[ci], colNullMasks_[ci]);
                RowContainer::StoreValue<int64_t>(row, colOffsets_[ci], cols[ci].int64Data[rowIdx]);
            }
        }
    }

    // ─── GetUnequalsNumWithDecode ────────────────────────────────────────────

    int32_t GetUnequalsNumWithDecode(int32_t count, const std::vector<ColumnInput>& columns) {
        mergedVarcharCache_.clear();
        mergedVarcharCacheCount_ = 0;
        if (useMerged_) {
            mergedVarcharCacheCount_ = static_cast<int32_t>(varcharColIndices_.size());
            int32_t maxIdx = 0;
            for (int32_t w = 0; w < count; w++)
                maxIdx = std::max(maxIdx, workingUpdateIndices_[w]);
            mergedVarcharCache_.resize(static_cast<size_t>(maxIdx + 1) * mergedVarcharCacheCount_, nullptr);
            for (int32_t w = 0; w < count; w++) {
                int32_t idx = workingUpdateIndices_[w];
                GetAllMergedVarcharPtrs(
                    reinterpret_cast<const char*>(groups_[idx]),
                    &mergedVarcharCache_[static_cast<size_t>(idx) * mergedVarcharCacheCount_],
                    mergedVarcharCacheCount_);
            }
        }

        int32_t idxFrom = 0;
        for (int32_t colIdx = 0; colIdx < groupColNum_ && idxFrom < count; colIdx++) {
            int32_t offset = colOffsets_[colIdx];
            if (colDescs_[colIdx] == ColumnDesc::Int64) {
                const int64_t* inputValues = columns[colIdx].int64Data;
#ifdef __ARM_NEON
                // NEON: compare 2 × i64 per iteration (matches Rust batch_compare_decoded_i64_neon)
                int32_t i = idxFrom;
                for (; i + 2 <= count; i += 2) {
                    int32_t idx0 = workingUpdateIndices_[i];
                    int32_t idx1 = workingUpdateIndices_[i + 1];
                    int64_t stored0 = RowContainer::ReadValue<int64_t>(reinterpret_cast<const char*>(groups_[idx0]), offset);
                    int64_t stored1 = RowContainer::ReadValue<int64_t>(reinterpret_cast<const char*>(groups_[idx1]), offset);
                    int64_t input0 = inputValues[idx0];
                    int64_t input1 = inputValues[idx1];

                    int64x2_t vStored = vcombine_s64(vcreate_s64(static_cast<uint64_t>(stored0)),
                                                     vcreate_s64(static_cast<uint64_t>(stored1)));
                    int64x2_t vInput = vcombine_s64(vcreate_s64(static_cast<uint64_t>(input0)),
                                                    vcreate_s64(static_cast<uint64_t>(input1)));
                    uint64x2_t cmp = vceqq_s64(vStored, vInput);

                    if (vgetq_lane_u64(cmp, 0) == 0) { // not equal
                        std::swap(workingUpdateIndices_[i], workingUpdateIndices_[idxFrom]);
                        idxFrom++;
                    }
                    if (vgetq_lane_u64(cmp, 1) == 0) { // not equal
                        std::swap(workingUpdateIndices_[i + 1], workingUpdateIndices_[idxFrom]);
                        idxFrom++;
                    }
                }
                // Scalar tail
                for (; i < count; i++) {
                    int32_t idx = workingUpdateIndices_[i];
                    if (RowContainer::ReadValue<int64_t>(reinterpret_cast<const char*>(groups_[idx]), offset) != inputValues[idx]) {
                        std::swap(workingUpdateIndices_[i], workingUpdateIndices_[idxFrom]); idxFrom++;
                    }
                }
#else
                for (int32_t i = idxFrom; i < count; i++) {
                    int32_t idx = workingUpdateIndices_[i];
                    if (RowContainer::ReadValue<int64_t>(reinterpret_cast<const char*>(groups_[idx]), offset) != inputValues[idx]) {
                        std::swap(workingUpdateIndices_[i], workingUpdateIndices_[idxFrom]); idxFrom++;
                    }
                }
#endif
            } else {
                BatchCompareVarcharDecodedNoNull(colIdx, count, offset, columns[colIdx].vcSlices, workingUpdateIndices_.data(), idxFrom);
            }
        }
        return idxFrom;
    }

    void BatchCompareVarcharDecodedNoNull(int32_t colIdx, int32_t count, int32_t offset,
                                          const VarcharSlice* inputSlices, int32_t* indices, int32_t& idxFrom)
    {
        const int32_t vcPos = colToVarcharPos_[colIdx];
        for (int32_t i = idxFrom; i < count; i++) {
            int32_t idx = indices[i];
            const uint8_t* arenaPtr;
            if (!mergedVarcharCache_.empty()) {
                arenaPtr = mergedVarcharCache_[static_cast<size_t>(idx) * mergedVarcharCacheCount_ + vcPos];
            } else {
                memcpy(&arenaPtr, reinterpret_cast<const char*>(groups_[idx]) + offset, sizeof(arenaPtr));
            }
            if (!arenaPtr || !CompareVarcharFromRow(arenaPtr, inputSlices[idx].ptr, inputSlices[idx].len)) {
                std::swap(indices[i], indices[idxFrom]);
                idxFrom++;
            }
        }
    }

    void GetAllMergedVarcharPtrs(const char* row, const uint8_t** outPtrs, int32_t maxCount) const {
        const uint8_t* blockPtr;
        memcpy(&blockPtr, row + varcharSlotOffset_, sizeof(blockPtr));
        if (!blockPtr) { memset(outPtrs, 0, maxCount * sizeof(uint8_t*)); return; }
        const uint8_t* pos = blockPtr;
        for (int32_t i = 0; i < maxCount; i++) {
            int32_t ci = varcharColIndices_[i];
            if (RowContainer::IsNullAt(row, colNullBytes_[ci], colNullMasks_[ci])) {
                outPtrs[i] = nullptr; pos += 1;
            } else {
                outPtrs[i] = pos; pos += ComputeVarCharSerializedSize(pos);
            }
        }
    }

    bool CompareKeysWithDecode(const uint8_t* rowPtr, int32_t rowIdx,
        const std::vector<ColumnInput>& columns) const
    {
        const char* row = reinterpret_cast<const char*>(rowPtr);
        for (int32_t colIdx = 0; colIdx < groupColNum_; colIdx++) {
            if (colDescs_[colIdx] == ColumnDesc::Int64) {
                if (RowContainer::ReadValue<int64_t>(row, colOffsets_[colIdx]) != columns[colIdx].int64Data[rowIdx])
                    return false;
            } else {
                const uint8_t* arenaPtr;
                memcpy(&arenaPtr, row + colOffsets_[colIdx], sizeof(arenaPtr));
                if (!arenaPtr || !CompareVarcharFromRow(arenaPtr, columns[colIdx].vcSlices[rowIdx].ptr, columns[colIdx].vcSlices[rowIdx].len))
                    return false;
            }
        }
        return true;
    }
};

} // namespace taper
