#pragma once
// pulseproxy/memory_arena.hpp — fixed-block buffer pool with zero heap
//                               allocation on the hot network path.
//
// DESIGN
// ──────
// The arena pre-allocates ARENA_POOL_CAPACITY blocks of ARENA_BLOCK_SIZE
// bytes at construction.  acquire() hands out a raw pointer to one block;
// release() returns it to the free-list.  Both operations are O(1) and
// allocation-free after construction.
//
// Thread safety: a single arena is NOT shared across threads.  Each worker
// thread owns its own MemoryArena.  This eliminates all locking overhead on
// the hot path.  Cross-thread operations are not needed: a channel is pinned
// to one worker for its entire lifetime.
//
// Memory layout
// ─────────────
//   [ Block 0 | Block 1 | ... | Block N-1 ]   ← one contiguous mmap/malloc
//   free_list_ = { &Block[N-1], ..., &Block[0] }  ← LIFO stack
//
// Each block is 64-KiB aligned (alignas(64)) so it starts on a cache-line
// boundary regardless of block size, avoiding false sharing between adjacent
// blocks that happen to straddle a cache line.

#include "pulseproxy/types.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pp {

class MemoryArena {
public:
    explicit MemoryArena(std::size_t capacity = ARENA_POOL_CAPACITY);
    ~MemoryArena() = default;

    // Not copyable or movable — the free-list holds pointers into storage_.
    MemoryArena(const MemoryArena&)            = delete;
    MemoryArena& operator=(const MemoryArena&) = delete;
    MemoryArena(MemoryArena&&)                 = delete;
    MemoryArena& operator=(MemoryArena&&)      = delete;

    // ── Acquire / Release ────────────────────────────────────────────────

    // Hand out a pointer to one ARENA_BLOCK_SIZE block.
    // Returns nullptr if the pool is exhausted (the caller should stop
    // reading until a block is released by another operation).
    uint8_t* acquire() noexcept;

    // Return a block previously handed out by acquire().
    // Undefined behaviour if ptr was not returned by this arena.
    void release(uint8_t* ptr) noexcept;

    // ── Diagnostics ──────────────────────────────────────────────────────

    std::size_t capacity()  const noexcept { return capacity_; }
    std::size_t available() const noexcept { return free_top_; }
    std::size_t in_use()    const noexcept { return capacity_ - free_top_; }

private:
    // One pre-allocated block.  alignas(64) → cache-line aligned.
    struct alignas(64) Block {
        uint8_t data[ARENA_BLOCK_SIZE];
    };

    std::size_t          capacity_;             // total blocks
    std::unique_ptr<Block[]> storage_;          // contiguous backing store
    std::vector<uint8_t*>    free_list_;        // LIFO stack of free pointers
    std::size_t              free_top_ = 0;     // index of next slot to pop
};

} // namespace pp
