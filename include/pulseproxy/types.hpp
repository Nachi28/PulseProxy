#pragma once
// pulseproxy/types.hpp — common primitives used across all modules.
//
// Keep this header minimal: only enums, constants, and thin type aliases.
// No heavy includes, no logic.

#include <cstdint>
#include <cstddef>
#include <sys/socket.h>  // SOMAXCONN

namespace pp {

// File-descriptor alias — an int on Linux, but named so its intent is clear
// in function signatures.
using Fd = int;

static constexpr Fd INVALID_FD = -1;

// Maximum number of epoll events drained per epoll_wait() call.
// 128 events × ~40 B state each ≈ 5 kB — fits in L1.
static constexpr int EPOLL_MAX_EVENTS = 128;

// Number of bytes in a single arena block (must be a power of two).
// 64 kB matches a typical large TCP receive window and Linux pipe capacity.
static constexpr std::size_t ARENA_BLOCK_SIZE = 65536;

// Number of blocks pre-allocated in the MemoryArena free-list.
static constexpr std::size_t ARENA_POOL_CAPACITY = 256;

// Backlog passed to listen(). SOMAXCONN defers to the kernel maximum.
static constexpr int LISTEN_BACKLOG = SOMAXCONN;

// TCP_NODELAY — disable Nagle. Mandatory for low-latency proxy work.
static constexpr bool DEFAULT_TCP_NODELAY = true;

// Channel state machine values.
enum class ChannelState : uint8_t {
    Connecting,  // upstream connect() in progress (non-blocking)
    Active,      // both sides open — forwarding data
    HalfClosed, // one side has sent FIN; draining the other
    Closed,     // both sides closed; safe to destroy
};

// Direction of a splice() transfer inside a Channel.
enum class Direction : uint8_t { ClientToUpstream, UpstreamToClient };

} // namespace pp
