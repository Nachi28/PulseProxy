#pragma once
// pulseproxy/channel.hpp — one bidirectional proxied connection.
//
// WHAT IS A CHANNEL?
// ──────────────────
// When a client connects, PulseProxy immediately opens a matching connection
// to the upstream backend.  A Channel owns both file descriptors and a pair
// of kernel pipes that sit between them:
//
//   client_fd  ──splice──▶  pipe_cu[1]  ──splice──▶  upstream_fd
//   upstream_fd──splice──▶  pipe_uc[1]  ──splice──▶  client_fd
//
// Data is never copied into user space.  splice() transfers page references
// between kernel socket buffers and the anonymous pipe, keeping the CPU out
// of the data path entirely.
//
// STATE MACHINE
// ─────────────
//   Connecting ──(upstream EPOLLOUT)──▶ Active
//   Active     ──(one side sends FIN)──▶ HalfClosed
//   HalfClosed ──(other side drained)──▶ Closed
//   Any        ──(error)──────────────▶ Closed
//
// OWNERSHIP
// ─────────
// EpollEngine creates and destroys Channels.  Channels never outlive the
// engine that owns them.  File descriptors are closed in the destructor.

#include "pulseproxy/types.hpp"
#include "pulseproxy/memory_arena.hpp"
#include <chrono>
#include <netinet/in.h>
#include <string>
#include <cstdint>
#include <cstddef>

namespace pp {

class Channel {
public:
    // Construct a channel for an already-accepted client socket.
    // connect_nonblocking() has been called on upstream_fd but may not have
    // completed yet — state starts as Connecting.
    Channel(Fd client_fd, const sockaddr_in& peer_addr,
            Fd upstream_fd,
            Fd pipe_cu[2],  // client→upstream pipe
            Fd pipe_uc[2]); // upstream→client pipe

    ~Channel();

    // Not copyable — owns file descriptors.
    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;

    // ── State transitions ────────────────────────────────────────────────

    // Called when upstream EPOLLOUT fires while state == Connecting.
    // Returns false if the connection failed; channel moves to Closed.
    bool on_upstream_connected();

    // Forward data from client to upstream.
    // Returns false if the channel should be torn down.
    bool on_client_readable(MemoryArena& arena);

    // Forward data from upstream to client.
    // Returns false if the channel should be torn down.
    bool on_upstream_readable(MemoryArena& arena);

    // Called when EPOLLOUT fires on client_fd (write-ready after back-pressure).
    bool on_client_writable(MemoryArena& arena);

    // Called when EPOLLOUT fires on upstream_fd (write-ready after back-pressure).
    bool on_upstream_writable(MemoryArena& arena);

    // Initiate graceful teardown.  Closes both sides, sets state to Closed.
    void close() noexcept;

    // ── Accessors ────────────────────────────────────────────────────────

    Fd           client_fd()   const noexcept { return client_fd_; }
    Fd           upstream_fd() const noexcept { return upstream_fd_; }
    ChannelState state()       const noexcept { return state_; }
    bool         is_closed()   const noexcept { return state_ == ChannelState::Closed; }

    // Human-readable peer address for logging.
    const std::string& peer_str() const noexcept { return peer_str_; }

    // Activity tracking for idle timeout reclamation.
    void touch() noexcept { last_active_ = std::chrono::steady_clock::now(); }
    bool is_idle_longer_than(int seconds) const noexcept {
        if (seconds <= 0) return false;
        auto now = std::chrono::steady_clock::now();
        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - last_active_).count();
        return dur >= seconds;
    }

    // Bytes forwarded in each direction (monotonic counters).
    uint64_t bytes_to_upstream()  const noexcept { return bytes_to_upstream_; }
    uint64_t bytes_to_client()    const noexcept { return bytes_to_client_; }

private:
    // splice() data from src_fd through the pipe into dst_fd.
    // Returns bytes transferred, 0 on EOF, -1 on error/EAGAIN.
    ssize_t splice_forward(Fd src_fd, Fd pipe_read, Fd pipe_write, Fd dst_fd);

    Fd client_fd_   = INVALID_FD;
    Fd upstream_fd_ = INVALID_FD;

    // Kernel pipe: client → upstream
    Fd pipe_cu_[2] = {INVALID_FD, INVALID_FD};
    // Kernel pipe: upstream → client
    Fd pipe_uc_[2] = {INVALID_FD, INVALID_FD};

    ChannelState state_ = ChannelState::Connecting;
    std::string  peer_str_;
    std::chrono::steady_clock::time_point last_active_ = std::chrono::steady_clock::now();

    uint64_t bytes_to_upstream_ = 0;
    uint64_t bytes_to_client_   = 0;

    // Bytes sitting in each pipe not yet drained to the destination socket
    // (back-pressure accounting).
    std::size_t pending_cu_ = 0; // bytes in pipe_cu_
    std::size_t pending_uc_ = 0; // bytes in pipe_uc_
};

} // namespace pp
