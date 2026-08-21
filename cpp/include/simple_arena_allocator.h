/**
 * SimpleArenaAllocator — exact replica of OmniOperator's memory/simple_arena_allocator.h.
 *
 * Bump-pointer allocator with exponential growth up to linearGrowthThreshold,
 * then linear growth. Not thread-safe. No individual free.
 */
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

namespace taper {

class SimpleArenaAllocator {
public:
    explicit SimpleArenaAllocator(int64_t minChunkSize = 4096,
                                  uint32_t growthFactor = 2,
                                  int64_t linearGrowthThreshold = 512 * 1024)
        : minChunkSize_(minChunkSize),
          totalBytes_(0), usedBytes_(0), availBytes_(0), availBuf_(nullptr),
          continuousUsedMemoryBytes_(0), continuousUsed_(false),
          growthFactor_(growthFactor),
          linearGrowthThreshold_(linearGrowthThreshold) {}

    ~SimpleArenaAllocator() { ReleaseChunks(); }

    SimpleArenaAllocator(const SimpleArenaAllocator&) = delete;
    SimpleArenaAllocator& operator=(const SimpleArenaAllocator&) = delete;

    /// Reset arena for reuse: keep allocated chunks, reset bump pointer to start.
    /// Avoids free/malloc cycle and page fault overhead on reuse.
    void Reset() {
        if (!chunks_.empty()) {
            availBuf_ = chunks_[0];
            availBytes_ = chunkSizes_[0];
        } else {
            availBuf_ = nullptr;
            availBytes_ = 0;
        }
        // We only reuse the first (largest) chunk for simplicity.
        // Free remaining chunks and keep only the first one with total capacity.
        if (chunks_.size() > 1) {
            // Merge: allocate one big chunk = totalBytes_, free all old
            uint64_t total = totalBytes_;
            ReleaseChunks();
            AllocateChunk(total);
        }
        usedBytes_ = 0;
        continuousUsedMemoryBytes_ = 0;
        continuousUsed_ = false;
    }

    uint8_t* Allocate(int64_t sizeInBytes) {
        if (sizeInBytes == 0) {
            static int64_t zeroAddress[1];
            return reinterpret_cast<uint8_t*>(&zeroAddress);
        }
        if (availBytes_ < static_cast<uint64_t>(sizeInBytes)) {
            AllocateChunk(GetNextSize(static_cast<uint64_t>(sizeInBytes)));
        }
        uint8_t* ret = availBuf_;
        availBuf_ += sizeInBytes;
        availBytes_ -= sizeInBytes;
        return ret;
    }

    uint8_t* AllocateContinue(int64_t sizeInBytes, const uint8_t*& start) {
        continuousUsed_ = true;
        if (start == nullptr) {
            uint8_t* ret = Allocate(sizeInBytes);
            start = ret;
            return ret;
        }
        return AllocateContinueNotNull(sizeInBytes, start);
    }

    uint64_t TotalBytes() const { return totalBytes_; }
    uint64_t UsedBytes() const { return usedBytes_; }

private:
    uint64_t GetNextSize(uint64_t sizeInBytes) {
        if (chunks_.empty()) {
            return std::max(sizeInBytes, static_cast<uint64_t>(minChunkSize_));
        }
        auto lastChunkSize = chunkSizes_.back();
        if (lastChunkSize < static_cast<uint64_t>(linearGrowthThreshold_)) {
            return std::max(sizeInBytes, lastChunkSize * growthFactor_);
        } else {
            return ((sizeInBytes + linearGrowthThreshold_ - 1) / linearGrowthThreshold_) * linearGrowthThreshold_;
        }
    }

    void AllocateChunk(uint64_t sizeInBytes) {
        auto* buf = static_cast<uint8_t*>(malloc(sizeInBytes));
        chunks_.push_back(buf);
        chunkSizes_.push_back(sizeInBytes);
        availBuf_ = buf;
        availBytes_ = sizeInBytes;
        totalBytes_ += sizeInBytes;
    }

    void ReleaseChunks() {
        for (auto* p : chunks_) free(p);
        chunks_.clear();
        chunkSizes_.clear();
    }

    uint8_t* AllocateContinueNotNull(int64_t sizeInBytes, const uint8_t*& start) {
        auto newSpace = continuousUsedMemoryBytes_ + static_cast<uint64_t>(sizeInBytes);
        if (availBytes_ < static_cast<uint64_t>(sizeInBytes)) {
            AllocateChunk(GetNextSize(newSpace));
            memcpy(availBuf_, start, continuousUsedMemoryBytes_);
            start = availBuf_;
            availBuf_ += continuousUsedMemoryBytes_;
            availBytes_ -= continuousUsedMemoryBytes_;
        }
        uint8_t* ret = availBuf_;
        availBuf_ += sizeInBytes;
        continuousUsedMemoryBytes_ += sizeInBytes;
        availBytes_ -= sizeInBytes;
        usedBytes_ += sizeInBytes;
        return ret;
    }

    int64_t minChunkSize_;
    uint64_t totalBytes_, usedBytes_, availBytes_;
    uint8_t* availBuf_;
    uint64_t continuousUsedMemoryBytes_;
    bool continuousUsed_;
    uint32_t growthFactor_;
    int64_t linearGrowthThreshold_;
    std::vector<uint8_t*> chunks_;
    std::vector<uint64_t> chunkSizes_;
};

} // namespace taper
