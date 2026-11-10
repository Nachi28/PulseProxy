// pulseproxy/memory_arena.cpp
#include "pulseproxy/memory_arena.hpp"

#include <cassert>
#include <stdexcept>

namespace pp {

MemoryArena::MemoryArena(std::size_t capacity)
    : capacity_(capacity),
      storage_(std::make_unique<Block[]>(capacity)),
      free_list_(capacity),
      free_top_(capacity)
{
    // Populate the free-list in reverse order so the first acquire() returns
    // Block[0] (LIFO keeps data cache-warm for sequential allocate/release).
    for (std::size_t i = 0; i < capacity; ++i)
        free_list_[i] = storage_[capacity - 1 - i].data;
}

uint8_t* MemoryArena::acquire() noexcept {
    if (free_top_ == 0) return nullptr;      // pool exhausted
    return free_list_[--free_top_];
}

void MemoryArena::release(uint8_t* ptr) noexcept {
    assert(ptr != nullptr);
    assert(free_top_ < capacity_);           // double-free would overflow
    free_list_[free_top_++] = ptr;
}

} // namespace pp
