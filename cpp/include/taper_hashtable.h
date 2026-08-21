/**
 * Standalone TaperFlatHashTable — typed struct layout matching Rust Chunk exactly.
 *
 * - 128-byte aligned Chunk: tags[8] + keys[8] + pad[8] + values[8]
 * - PHBitMask SWAR tag matching
 * - 6-byte compressed row pointer (ROW_PTR_SIZE = 6)
 * - EmplaceBatch: precompute hash+positions → per-row TryEmplaceAtPos → collision iteration
 * - 0.9 load factor, prefetch
 * - Key = int64_t (pre-hashed), KeyScattered = true (Hash(key) = key)
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <vector>
#include <algorithm>
#include <cstdlib>

namespace taper {

// ═══════════════════════════════════════════════════════════════════════════════
// PHBitMask — SWAR tag matching
// ═══════════════════════════════════════════════════════════════════════════════

class PHBitMask {
    static constexpr int kShift = 3;
    uint64_t mask_;
public:
    explicit PHBitMask(uint64_t mask) : mask_(mask) {}
    PHBitMask& operator++() { mask_ &= (mask_ - 1); return *this; }
    explicit operator bool() const { return mask_ != 0; }
    uint32_t operator*() const { return __builtin_ctzll(mask_) >> kShift; }
    PHBitMask begin() const { return *this; }
    PHBitMask end() const { return PHBitMask(0); }
    friend bool operator!=(const PHBitMask& a, const PHBitMask& b) { return a.mask_ != b.mask_; }

    static inline PHBitMask MatchTag(uint64_t tagVal, uint8_t tagHash) {
        constexpr uint64_t kMsbs = 0x8080808080808080ULL;
        constexpr uint64_t kLsbs = 0x0101010101010101ULL;
        auto x = tagVal ^ (kLsbs * tagHash);
        return PHBitMask((x - kLsbs) & ~x & kMsbs);
    }
    static inline PHBitMask MatchEmpty(uint64_t tagVal) {
        constexpr uint64_t kMsbs = 0x8080808080808080ULL;
        return PHBitMask((tagVal & (~tagVal << 7)) & kMsbs);
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// Chunk — 128 bytes, typed struct matching Rust exactly
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr uint8_t kEmptyTag = 0x80;
static constexpr size_t kHashMapPrefetchDist = 16;
static constexpr uint32_t ROW_PTR_SIZE = 6;
static constexpr uint32_t kSlotsPerChunk = 8;

/// 6-byte compressed pointer — matches Rust SlotValue.
struct SlotValue {
    char bytes[ROW_PTR_SIZE];
};

/// 128-byte chunk with typed fields — matches Rust Chunk layout exactly:
///   tags:   [u8; 8]       @ 0
///   keys:   [u64; 8]      @ 8
///   _pad:   [u8; 8]       @ 72
///   values: [SlotValue; 8] @ 80
///   Total: 128 bytes
struct alignas(128) Chunk {
    uint8_t tags[kSlotsPerChunk];          // offset 0,  8 bytes
    uint64_t keys[kSlotsPerChunk];         // offset 8,  64 bytes
    uint8_t _pad[8];                       // offset 72, 8 bytes
    SlotValue values[kSlotsPerChunk];      // offset 80, 48 bytes

    uint64_t TagsU64() const { uint64_t v; memcpy(&v, tags, 8); return v; }
};
static_assert(sizeof(Chunk) == 128);
static_assert(offsetof(Chunk, tags) == 0);
static_assert(offsetof(Chunk, keys) == 8);
static_assert(offsetof(Chunk, values) == 80);

// ═══════════════════════════════════════════════════════════════════════════════
// TaperFlatHashTable
// ═══════════════════════════════════════════════════════════════════════════════

class TaperFlatHashTable {
public:
    using Key = int64_t;
    using ChunkPos = uint32_t;

    explicit TaperFlatHashTable(size_t initialChunks = 1) {
        size_t n = 1; while (n < initialChunks) n <<= 1;
        AllocChunks(static_cast<uint32_t>(n - 1));
    }

    ~TaperFlatHashTable() { FreeChunks(); }

    size_t Size() const { return size_; }
    size_t Capacity() const { return (static_cast<size_t>(lastChunkIdx_) + 1) * kSlotsPerChunk; }
    uint32_t LastChunkIdx() const { return lastChunkIdx_; }
    const Chunk* ChunkAt(size_t idx) const { return chunks_ + idx; }

    // ─── EmplaceBatch ───────────────────────────────────────────────

    template <typename Filter, typename FInit, typename FUpdate>
    void EmplaceBatch(const Key* keys, int32_t numRows, Filter&& filter, FInit&& fInit, FUpdate&& fUpdate) {
        if (Capacity() < static_cast<size_t>(numRows)) {
            EmplaceBatchDirectly(keys, numRows, std::forward<Filter>(filter),
                std::forward<FInit>(fInit), std::forward<FUpdate>(fUpdate));
            return;
        }
        // ResetEmplaceContext
        emplaceHashVals_.resize(numRows);
        emplacePositions_.resize(numRows);
        emplaceCollisions_.resize(numRows);
        for (int32_t i = 0; i < numRows; i++) {
            uint64_t val = Hash(keys[i]);
            emplaceHashVals_[i] = val;
            emplacePositions_[i] = GetChunkPos(val);
        }

        uint32_t collisionBatch = 1;
        int32_t collisionCount = 0;

        auto resetPositions = [&](int32_t begin, int32_t end) {
            for (int32_t i = begin; i < end; i++)
                emplacePositions_[i] = GetChunkPos(emplaceHashVals_[i]);
        };
        auto tryEmplaceRehashedCollisions = [&] {
            resetPositions(0, collisionCount);
            int32_t curCount = collisionCount; collisionCount = 0;
            for (int32_t idx = 0; idx < curCount; idx++) {
                PrefetchIdx(idx, curCount);
                uint32_t rowIdx = emplaceCollisions_[idx];
                TryEmplaceAtPos(keys[rowIdx], emplaceHashVals_[idx], emplacePositions_[idx],
                    [&](char* d) { fInit(rowIdx, d); },
                    [&](char* d, bool f) { fUpdate(rowIdx, d, f); });
            }
        };
        auto resizeProc = [&](int32_t remainFrom, int32_t remainTo) {
            collisionBatch = 1;
            tryEmplaceRehashedCollisions();
            resetPositions(remainFrom, remainTo);
        };

        // First pass
        for (int32_t i = 0; i < numRows; ++i) {
            if (filter(i)) continue;
            PrefetchIdx(i, numRows);
            bool ok = TryEmplaceAtPos(keys[i], emplaceHashVals_[i], emplacePositions_[i],
                [&](char* d) { fInit(i, d); },
                [&](char* d, bool f) { fUpdate(i, d, f); });
            if (!ok) {
                emplaceCollisions_[collisionCount] = i;
                emplaceHashVals_[collisionCount] = emplaceHashVals_[i];
                emplacePositions_[collisionCount] = GetRehashPos(collisionBatch, emplacePositions_[i]);
                collisionCount++;
                if (ShouldExpand()) { ExpandCapacity(); resizeProc(i + 1, numRows); }
            }
        }
        // Collision iteration
        while (collisionCount > 0) {
            int32_t curCount = collisionCount; collisionCount = 0; collisionBatch++;
            for (int32_t idx = 0; idx < curCount; idx++) {
                PrefetchIdx(idx, curCount);
                uint32_t rowIdx = emplaceCollisions_[idx];
                bool ok = TryEmplaceAtPos(keys[rowIdx], emplaceHashVals_[idx], emplacePositions_[idx],
                    [&](char* d) { fInit(rowIdx, d); },
                    [&](char* d, bool f) { fUpdate(rowIdx, d, f); });
                if (!ok) {
                    emplaceCollisions_[collisionCount] = rowIdx;
                    emplaceHashVals_[collisionCount] = emplaceHashVals_[idx];
                    emplacePositions_[collisionCount] = GetRehashPos(collisionBatch, emplacePositions_[idx]);
                    collisionCount++;
                    if (ShouldExpand()) {
                        ExpandCapacity();
                        collisionBatch = 1;
                        tryEmplaceRehashedCollisions();
                        resetPositions(idx + 1, curCount);
                    }
                }
            }
        }
    }

    // ─── Scalar Emplace (Step 5 fallback) ───────────────────────────

    template <typename FKeyCmp, typename FInit, typename FUpdate>
    void Emplace(Key key, FKeyCmp&& fKeyCmp, FInit&& fInit, FUpdate&& fUpdate) {
        uint64_t hashVal = Hash(key);
        ChunkPos chunkPos = GetChunkPos(hashVal);
        size_t collisionBatch = 1;
        while (true) {
            auto* chunk = chunks_ + chunkPos;
            uint8_t tagHash = (hashVal >> 16) & 0x7F;
            auto tags = chunk->TagsU64();
            for (auto it = PHBitMask::MatchTag(tags, tagHash); it; ++it) {
                uint32_t slot = *it;
                if (chunk->keys[slot] == static_cast<uint64_t>(key) && fKeyCmp(chunk->values[slot].bytes)) {
                    fUpdate(chunk->values[slot].bytes, false); return;
                }
            }
            for (auto it = PHBitMask::MatchEmpty(tags); it; ++it) {
                uint32_t slot = *it;
                size_++; chunk->tags[slot] = tagHash;
                chunk->keys[slot] = static_cast<uint64_t>(key);
                fInit(chunk->values[slot].bytes); fUpdate(chunk->values[slot].bytes, true); return;
            }
            if (ShouldExpand()) { ExpandCapacity(); chunkPos = GetChunkPos(hashVal); collisionBatch = 1; }
            else { chunkPos = GetRehashPos(collisionBatch, chunkPos); collisionBatch++; }
        }
    }

private:
    uint64_t Hash(Key key) const { return static_cast<uint64_t>(key); } // KeyScattered=true
    ChunkPos GetChunkPos(uint64_t h) const { return h & lastChunkIdx_; }
    ChunkPos GetRehashPos(size_t batch, ChunkPos pos) const { return (pos + batch) & lastChunkIdx_; }
    bool ShouldExpand() const { return size_ >= expandThreshold_; }

    template <typename FInit, typename FUpdate>
    bool TryEmplaceAtPos(Key key, uint64_t hashVal, ChunkPos chunkPos, FInit&& fInit, FUpdate&& fUpdate) {
        auto* chunk = chunks_ + chunkPos;
        uint8_t tagHash = (hashVal >> 16) & 0x7F;
        auto tags = chunk->TagsU64();
        // Tag match → KeyEquals (int64 ==)
        for (auto it = PHBitMask::MatchTag(tags, tagHash); it; ++it) {
            uint32_t slot = *it;
            if (chunk->keys[slot] == static_cast<uint64_t>(key)) {
                fUpdate(chunk->values[slot].bytes, false); return true;
            }
        }
        // Empty slot
        for (auto it = PHBitMask::MatchEmpty(tags); it; ++it) {
            uint32_t slot = *it;
            size_++; chunk->tags[slot] = tagHash;
            chunk->keys[slot] = static_cast<uint64_t>(key);
            char* vb = chunk->values[slot].bytes;
            fInit(vb); fUpdate(vb, true); return true;
        }
        return false;
    }

    void Prefetch(ChunkPos pos) const {
        auto* p = reinterpret_cast<const char*>(chunks_ + pos);
        __builtin_prefetch(p); __builtin_prefetch(p + 64);
    }
    void PrefetchIdx(int32_t idx, int32_t end) const {
        auto pi = idx + static_cast<int32_t>(kHashMapPrefetchDist);
        if (pi < end) Prefetch(emplacePositions_[pi]);
    }

    void AllocChunks(uint32_t lastChunkIdx) {
        auto cap = static_cast<size_t>(lastChunkIdx) + 1;
        size_t bytes = cap * sizeof(Chunk);
        chunks_ = static_cast<Chunk*>(aligned_alloc(128, bytes));
        memset(chunks_, kEmptyTag, bytes);
        lastChunkIdx_ = lastChunkIdx; size_ = 0;
        expandThreshold_ = static_cast<uint32_t>(cap * kSlotsPerChunk * 9 / 10);
    }
    void FreeChunks() { if (chunks_) { free(chunks_); chunks_ = nullptr; } }
    uint32_t ExpandLastChunkIdx() const { return 2 * lastChunkIdx_ + 1; }

    // ─── Expand: collect-all + batch insert with prefetch (matches Rust) ───

    void ExpandCapacity() {
        auto oldNum = static_cast<size_t>(lastChunkIdx_) + 1;
        auto* oldChunks = chunks_;

        // Collect all occupied entries from old chunks
        struct Entry { uint64_t key; SlotValue val; };
        std::vector<Entry> entries;
        entries.reserve(size_);
        for (size_t ci = 0; ci < oldNum; ci++) {
            auto* c = oldChunks + ci;
            for (uint32_t s = 0; s < kSlotsPerChunk; s++) {
                if (c->tags[s] != kEmptyTag) {
                    Entry e; e.key = c->keys[s]; e.val = c->values[s];
                    entries.push_back(e);
                }
            }
        }

        // Allocate new chunk array (2x)
        AllocChunks(ExpandLastChunkIdx());
        free(oldChunks);

        size_t n = entries.size();
        if (n == 0) return;

        // Compute initial positions
        std::vector<ChunkPos> positions(n);
        for (size_t i = 0; i < n; i++) positions[i] = GetChunkPos(entries[i].key);

        // Batch insert with collision iteration + prefetch (matches Rust expand)
        std::vector<size_t> active(n);
        for (size_t i = 0; i < n; i++) active[i] = i;
        size_t collisionBatch = 1;

        while (!active.empty()) {
            for (size_t pi = 0; pi < std::min(active.size(), kHashMapPrefetchDist); pi++) {
                Prefetch(positions[active[pi]]);
            }
            std::vector<size_t> collisions;
            for (size_t idx = 0; idx < active.size(); idx++) {
                size_t pi = idx + kHashMapPrefetchDist;
                if (pi < active.size()) Prefetch(positions[active[pi]]);

                size_t ei = active[idx];
                auto& e = entries[ei];
                ChunkPos pos = positions[ei];
                uint8_t tag = (e.key >> 16) & 0x7F;
                auto* chunk = chunks_ + pos;
                bool inserted = false;
                for (auto it = PHBitMask::MatchEmpty(chunk->TagsU64()); it; ++it) {
                    uint32_t slot = *it;
                    size_++; chunk->tags[slot] = tag;
                    chunk->keys[slot] = e.key;
                    chunk->values[slot] = e.val;
                    inserted = true; break;
                }
                if (!inserted) {
                    positions[ei] = GetRehashPos(collisionBatch, pos);
                    collisions.push_back(ei);
                }
            }
            active = std::move(collisions);
            collisionBatch++;
        }
    }

    template <typename Filter, typename FInit, typename FUpdate>
    void EmplaceBatchDirectly(const Key* keys, int32_t numRows, Filter&& filter, FInit&& fInit, FUpdate&& fUpdate) {
        for (int32_t i = 0; i < numRows; ++i) {
            if (filter(i)) continue;
            uint64_t h = Hash(keys[i]); ChunkPos pos = GetChunkPos(h); size_t cb = 1;
            while (true) {
                bool ok = TryEmplaceAtPos(keys[i], h, pos,
                    [&](char* d) { fInit(i, d); }, [&](char* d, bool f) { fUpdate(i, d, f); });
                if (ok) break;
                if (ShouldExpand()) { ExpandCapacity(); pos = GetChunkPos(h); cb = 1; }
                else { pos = GetRehashPos(cb, pos); cb++; }
            }
        }
    }

    Chunk* chunks_ = nullptr;
    uint32_t size_ = 0, lastChunkIdx_ = 0, expandThreshold_ = 0;
    std::vector<uint64_t> emplaceHashVals_;
    std::vector<ChunkPos> emplacePositions_;
    std::vector<uint32_t> emplaceCollisions_;
};

} // namespace taper
