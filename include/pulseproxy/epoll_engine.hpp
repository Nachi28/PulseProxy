#pragma once
// pulseproxy/epoll_engine.hpp — the event loop that runs one worker thread.
//
// OVERVIEW
// ────────
// EpollEngine owns:
//   • One epoll(7) instance (edge-triggered, EPOLLET).
//   • One listener socket (or a SO_REUSEPORT clone per worker).
//   • The MemoryArena for this thread's buffer allocations.
//   • A flat map from file descriptor → Channel* for O(1) dispatch.
//
// The public interface is just run() — it blocks until stop() is called from
// any thread.  It is designed to be constructed and run on a worker thread:
//
//   EpollEngine engine(config, worker_id);
//   std::thread t([&engine]{ engine.run(); });
//   // ... later ...
//   engine.stop();
//   t.join();
//
// SCALABILITY MODEL
// ─────────────────
// When config.reuseport == true (the default), every EpollEngine creates its
// own listener socket bound to the same port with SO_REUSEPORT.  The kernel
// then distributes incoming SYN packets across all listeners using a 4-tuple
// hash — no mutex, no thundering herd, perfect CPU locality.
//
// When reuseport == false (useful for testing or single-threaded mode), one
// listener fd is created externally and shared across engines.
//
// CHANNEL LIFECYCLE
// ─────────────────
//   accept4()  → create Channel (Connecting)
//   EPOLLOUT on upstream_fd → Channel::on_upstream_connected()
//   EPOLLIN  on client_fd   → Channel::on_client_readable()
//   EPOLLIN  on upstream_fd → Channel::on_upstream_readable()
//   channel.is_closed()     → deregister fds, free Channel

#include "pulseproxy/config.hpp"
#include "pulseproxy/types.hpp"
#include "pulseproxy/memory_arena.hpp"
#include "pulseproxy/channel.hpp"

#include <sys/epoll.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace pp {

class EpollEngine {
public:
    // shared_listener_fd == INVALID_FD  → engine creates its own (SO_REUSEPORT).
    // shared_listener_fd != INVALID_FD  → engine uses it directly (single-thread mode).
    EpollEngine(const Config& cfg, int worker_id,
                Fd shared_listener_fd = INVALID_FD);
    ~EpollEngine();

    // Not copyable or movable.
    EpollEngine(const EpollEngine&)            = delete;
    EpollEngine& operator=(const EpollEngine&) = delete;

    // Block and process events until stop() is called.
    void run();

    // Signal the loop to exit cleanly.  Thread-safe.
    void stop() noexcept;

    // ── Diagnostics ──────────────────────────────────────────────────────

    int      worker_id()          const noexcept { return worker_id_; }
    uint64_t connections_total()  const noexcept { return conns_total_.load(std::memory_order_relaxed); }
    uint64_t connections_active() const noexcept { return conns_active_.load(std::memory_order_relaxed); }
    uint64_t bytes_proxied()      const noexcept { return bytes_proxied_.load(std::memory_order_relaxed); }

private:
    // ── Helpers ──────────────────────────────────────────────────────────

    void accept_new_connections();
    void dispatch_event(const struct epoll_event& ev);
    void register_channel(Channel* ch);
    void deregister_and_free(Channel* ch);
    void sweep_idle_channels();

    // Add/modify/remove fd in the epoll interest list.
    void epoll_add(Fd fd, uint32_t events, void* ptr);
    void epoll_mod(Fd fd, uint32_t events, void* ptr);
    void epoll_del(Fd fd) noexcept;

    // ── Members ──────────────────────────────────────────────────────────

    const Config& cfg_;
    int           worker_id_;

    Fd epoll_fd_    = INVALID_FD;
    Fd listen_fd_   = INVALID_FD;   // our own listener (or shared)
    bool owns_listener_ = false;    // true → we close it on destruction

    // eventfd used by stop() to wake the blocked epoll_wait().
    Fd stop_fd_     = INVALID_FD;

    std::atomic<bool> stop_requested_{false};

    // Per-thread buffer pool — zero shared state with other workers.
    MemoryArena arena_;

    // Direct flat array for O(1) FD lookup (index = fd).
    // Zero hash overhead, zero collisions, maximum cache locality.
    std::vector<Channel*> fd_table_;
    std::vector<Channel*> active_channels_;
    std::chrono::steady_clock::time_point last_sweep_ = std::chrono::steady_clock::now();

    // Monotonic counters updated after each event.
    std::atomic<uint64_t> conns_total_{0};
    std::atomic<uint64_t> conns_active_{0};
    std::atomic<uint64_t> bytes_proxied_{0};
};

} // namespace pp
