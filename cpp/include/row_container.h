/**
 * Standalone RowContainer — faithful to OmniOperator's RowContainer.
 * Uses SimpleArenaAllocator (external, passed by reference) exactly as OmniOperator does.
 * Target: Linux aarch64.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <cassert>
#include <vector>
#include <cstdlib>
#include "simple_arena_allocator.h"

namespace taper {

class RowColumn {
    uint64_t packed_;
public:
    RowColumn() : packed_(0) {}
    RowColumn(int32_t offset, int32_t nullBitOffset, int32_t nullBlockStart) {
        int32_t nullByte = nullBlockStart + nullBitOffset / 8;
        uint8_t nullMask = 1u << (nullBitOffset & 7);
        packed_ = (static_cast<uint64_t>(static_cast<uint32_t>(offset)) << 32) |
                  (static_cast<uint64_t>(static_cast<uint32_t>(nullByte)) << 8) | nullMask;
    }
    int32_t Offset() const { return static_cast<int32_t>(packed_ >> 32); }
    uint32_t NullByte() const { return static_cast<uint32_t>((packed_ >> 8) & 0x00FFFFFF); }
    uint8_t NullMask() const { return static_cast<uint8_t>(packed_ & 0xFF); }
};

enum class ColumnKind { Fixed, Varchar };
static constexpr size_t kVarcharSlotSize = sizeof(void*);
static constexpr int32_t kBatchSize = 1024;

class RowContainer {
public:
    /// Constructor: takes external SimpleArenaAllocator reference (same as OmniOperator).
    RowContainer(const std::vector<size_t>& keySizes,
                 const std::vector<ColumnKind>& kinds,
                 size_t aggStateSize,
                 SimpleArenaAllocator& pool)
        : pool_(pool)
    {
        numKeys_ = static_cast<int32_t>(keySizes.size());
        int32_t cur = 0;
        for (size_t i = 0; i < keySizes.size(); i++) {
            offsets_.push_back(cur);
            cur += (kinds[i] == ColumnKind::Varchar) ? static_cast<int32_t>(kVarcharSlotSize) : static_cast<int32_t>(keySizes[i]);
        }
        nullBlockStart_ = cur;
        int32_t nullBytes = (numKeys_ + 7) / 8;
        aggStateOffset_ = nullBlockStart_ + nullBytes;
        fixedRowSize_ = aggStateOffset_ + static_cast<int32_t>(aggStateSize);
        for (int32_t i = 0; i < numKeys_; i++)
            rowColumns_.emplace_back(offsets_[i], i, nullBlockStart_);
    }

    /// NewRow: allocate from arena pool (same as OmniOperator RowContainer::NewRow)
    char* NewRow() {
        if (batchRemaining_ <= 0) {
            size_t sz = static_cast<size_t>(fixedRowSize_) * kBatchSize;
            batchPtr_ = reinterpret_cast<char*>(pool_.Allocate(static_cast<int64_t>(sz)));
            memset(batchPtr_, 0, sz);
            batchRemaining_ = kBatchSize;
        }
        char* r = batchPtr_; batchPtr_ += fixedRowSize_; batchRemaining_--; numRows_++; return r;
    }

    /// ArenaAlloc: allocate varchar data from the same pool (same as OmniOperator)
    uint8_t* ArenaAlloc(size_t size) {
        return pool_.Allocate(static_cast<int64_t>(size));
    }

    /// Pool access (for TaperHashMap chunk allocation if needed)
    SimpleArenaAllocator& Pool() { return pool_; }

    static bool IsNullAt(const char* r, uint32_t nb, uint8_t nm) { return (reinterpret_cast<const uint8_t*>(r)[nb] & nm) != 0; }
    static void SetNullAt(char* r, uint32_t nb, uint8_t nm) { reinterpret_cast<uint8_t*>(r)[nb] |= nm; }
    static void ClearNullAt(char* r, uint32_t nb, uint8_t nm) { reinterpret_cast<uint8_t*>(r)[nb] &= ~nm; }
    template<typename T> static T ReadValue(const char* r, int32_t off) { T v; memcpy(&v, r+off, sizeof(T)); return v; }
    template<typename T> static void StoreValue(char* r, int32_t off, T v) { memcpy(r+off, &v, sizeof(T)); }

    RowColumn ColumnAt(int32_t i) const { return rowColumns_[i]; }
    int32_t AggStateOffset() const { return aggStateOffset_; }
    int32_t FixedRowSize() const { return fixedRowSize_; }
    int64_t NumRows() const { return numRows_; }

private:
    SimpleArenaAllocator& pool_;
    int32_t numKeys_=0, fixedRowSize_=0, nullBlockStart_=0, aggStateOffset_=0;
    int64_t numRows_=0;
    std::vector<int32_t> offsets_;
    std::vector<RowColumn> rowColumns_;
    char* batchPtr_=nullptr; int32_t batchRemaining_=0;
};

} // namespace taper
