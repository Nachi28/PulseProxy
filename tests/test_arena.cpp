// Tests for MemoryArena — the buffer pool used on the hot network path.
#include "test_framework.hpp"
#include "pulseproxy/memory_arena.hpp"

using namespace pp;

PP_TEST(arena_basic_acquire_release) {
    MemoryArena arena(8);
    PP_EXPECT_EQ(arena.capacity(),  8u);
    PP_EXPECT_EQ(arena.available(), 8u);
    PP_EXPECT_EQ(arena.in_use(),    0u);

    uint8_t* b = arena.acquire();
    PP_EXPECT(b != nullptr);
    PP_EXPECT_EQ(arena.available(), 7u);
    PP_EXPECT_EQ(arena.in_use(),    1u);

    arena.release(b);
    PP_EXPECT_EQ(arena.available(), 8u);
    PP_EXPECT_EQ(arena.in_use(),    0u);
}

PP_TEST(arena_exhaust_then_recover) {
    MemoryArena arena(4);
    uint8_t* ptrs[4];

    // Drain the pool completely.
    for (int i = 0; i < 4; ++i) {
        ptrs[i] = arena.acquire();
        PP_EXPECT(ptrs[i] != nullptr);
    }
    PP_EXPECT_EQ(arena.available(), 0u);

    // Next acquire must return nullptr — no allocation from the heap.
    uint8_t* overflow = arena.acquire();
    PP_EXPECT(overflow == nullptr);

    // Release all and confirm full recovery.
    for (int i = 0; i < 4; ++i) arena.release(ptrs[i]);
    PP_EXPECT_EQ(arena.available(), 4u);
}

PP_TEST(arena_blocks_are_distinct) {
    MemoryArena arena(16);
    // Acquire all blocks and verify no two are the same pointer.
    uint8_t* ptrs[16];
    for (int i = 0; i < 16; ++i) ptrs[i] = arena.acquire();
    for (int i = 0; i < 16; ++i)
        for (int j = i + 1; j < 16; ++j)
            PP_EXPECT_NE(ptrs[i], ptrs[j]);
    for (int i = 0; i < 16; ++i) arena.release(ptrs[i]);
}

PP_TEST(arena_block_size_is_correct) {
    // Each block must be exactly ARENA_BLOCK_SIZE bytes and cache-line aligned.
    MemoryArena arena(2);
    uint8_t* a = arena.acquire();
    uint8_t* b = arena.acquire();
    PP_EXPECT(a != nullptr);
    PP_EXPECT(b != nullptr);
    // Blocks must not overlap within ARENA_BLOCK_SIZE bytes.
    std::ptrdiff_t diff = a > b ? a - b : b - a;
    PP_EXPECT_GE(static_cast<std::size_t>(diff), ARENA_BLOCK_SIZE);
    arena.release(a);
    arena.release(b);
}

PP_TEST(arena_write_and_read_roundtrip) {
    // Basic sanity: bytes written to an acquired block survive until release.
    MemoryArena arena(1);
    uint8_t* block = arena.acquire();
    PP_EXPECT(block != nullptr);

    for (std::size_t i = 0; i < ARENA_BLOCK_SIZE; ++i)
        block[i] = static_cast<uint8_t>(i & 0xFF);

    for (std::size_t i = 0; i < ARENA_BLOCK_SIZE; ++i)
        PP_EXPECT_EQ(block[i], static_cast<uint8_t>(i & 0xFF));

    arena.release(block);
}
