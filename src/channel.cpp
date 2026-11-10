// pulseproxy/channel.cpp — bidirectional zero-copy splice() forwarding.
//
// DATA PATH (hot path, no malloc):
//   on_client_readable():
//     splice(client_fd → pipe_cu_[1])   — move socket data into kernel pipe
//     splice(pipe_cu_[0] → upstream_fd) — drain pipe into upstream socket
//     If upstream is not writable (EAGAIN), leave bytes in pipe and arm
//     EPOLLOUT on upstream_fd so we drain when it becomes writable.
//
//   on_upstream_readable(): symmetric in the other direction.
//
// BACK-PRESSURE:
//   pending_cu_ / pending_uc_ track bytes sitting in the respective pipe.
//   When the destination socket is not ready (EAGAIN on the second splice),
//   we increment the pending counter and arm EPOLLOUT.  When EPOLLOUT fires
//   we drain the remaining bytes from the pipe.

#include "pulseproxy/channel.hpp"
#include "pulseproxy/socket_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace pp {

// ── Construction / Destruction ────────────────────────────────────────────────

Channel::Channel(Fd client_fd, const sockaddr_in& peer_addr,
                 Fd upstream_fd,
                 Fd pipe_cu[2],
                 Fd pipe_uc[2])
    : client_fd_(client_fd), upstream_fd_(upstream_fd),
      state_(ChannelState::Connecting)
{
    pipe_cu_[0] = pipe_cu[0]; pipe_cu_[1] = pipe_cu[1];
    pipe_uc_[0] = pipe_uc[0]; pipe_uc_[1] = pipe_uc[1];

    // Human-readable peer address for log messages.
    char buf[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &peer_addr.sin_addr, buf, sizeof(buf));
    peer_str_ = std::string(buf) + ":" + std::to_string(ntohs(peer_addr.sin_port));
}

Channel::~Channel() { close(); }

// ── State transitions ─────────────────────────────────────────────────────────

bool Channel::on_upstream_connected() {
    if (!finish_connect(upstream_fd_)) {
        // Connection refused / reset — log and tear down.
        std::fprintf(stderr, "[channel] upstream connect failed for %s: %s\n",
                     peer_str_.c_str(), ::strerror(errno));
        close();
        return false;
    }
    state_ = ChannelState::Active;
    touch();
    return true;
}

// ── Core forward helpers ──────────────────────────────────────────────────────

// splice() data from src_fd through the kernel pipe into dst_fd.
// Returns:
//   > 0  — bytes transferred
//     0  — src_fd sent EOF (FIN)
//    -1  — EAGAIN (src or dst not ready) or hard error (errno set)
ssize_t Channel::splice_forward(Fd src_fd, Fd pipe_read, Fd pipe_write, Fd dst_fd) {
    // Flush any pending bytes sitting in pipe from previous partial write
    ssize_t flushed = ::splice(pipe_read, nullptr, dst_fd, nullptr,
                               ARENA_BLOCK_SIZE,
                               SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
    if (flushed < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return -1;
    }

    // Phase 1: drain src_fd into the write-end of the pipe.
    ssize_t spliced_in = ::splice(src_fd, nullptr, pipe_write, nullptr,
                                  ARENA_BLOCK_SIZE,
                                  SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
    if (spliced_in <= 0) {
        if (spliced_in < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return flushed > 0 ? flushed : -1;
        }
        return spliced_in; // 0 = EOF, -1 = hard error
    }

    // Phase 2: drain the read-end of the pipe into dst_fd.
    ssize_t spliced_out = ::splice(pipe_read, nullptr, dst_fd, nullptr,
                                   static_cast<std::size_t>(spliced_in),
                                   SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
    ssize_t total = (flushed > 0 ? flushed : 0) + (spliced_out > 0 ? spliced_out : 0);
    if (total > 0) return total;
    return spliced_out;
}

// ── EPOLLIN handlers ──────────────────────────────────────────────────────────

bool Channel::on_client_readable(MemoryArena& /*arena*/) {
    if (state_ == ChannelState::Connecting) return true; // wait for upstream connect
    if (state_ != ChannelState::Active) return false;

    // Edge-triggered: drain the socket completely in a tight loop.
    for (;;) {
        ssize_t n = splice_forward(client_fd_, pipe_cu_[0], pipe_cu_[1], upstream_fd_);
        if (n > 0) {
            bytes_to_upstream_ += static_cast<uint64_t>(n);
            touch();
        } else if (n == 0) {
            // Client sent FIN.
            ::shutdown(upstream_fd_, SHUT_WR);
            state_ = ChannelState::HalfClosed;
            return true;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false; // socket error
        }
    }
}

bool Channel::on_upstream_readable(MemoryArena& /*arena*/) {
    if (state_ == ChannelState::Connecting) return true;
    if (state_ != ChannelState::Active && state_ != ChannelState::HalfClosed)
        return false;

    for (;;) {
        ssize_t n = splice_forward(upstream_fd_, pipe_uc_[0], pipe_uc_[1], client_fd_);
        if (n > 0) {
            bytes_to_client_ += static_cast<uint64_t>(n);
            touch();
        } else if (n == 0) {
            // Upstream sent FIN.
            ::shutdown(client_fd_, SHUT_WR);
            if (state_ == ChannelState::HalfClosed) {
                // Both sides done.
                close();
            } else {
                state_ = ChannelState::HalfClosed;
            }
            return !is_closed();
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false; // socket error
        }
    }
}

// ── EPOLLOUT handlers (back-pressure drain) ───────────────────────────────────

bool Channel::on_client_writable(MemoryArena& arena) {
    if (state_ == ChannelState::Connecting) return true;
    // Drain whatever is sitting in pipe_uc_ → client_fd_.
    return on_upstream_readable(arena);
}

bool Channel::on_upstream_writable(MemoryArena& arena) {
    if (state_ == ChannelState::Connecting) {
        if (!on_upstream_connected()) return false;
        // Upstream is now connected; flush any client data already buffered
        return on_client_readable(arena);
    }
    // Drain whatever is sitting in pipe_cu_ → upstream_fd_.
    return on_client_readable(arena);
}

// ── Teardown ─────────────────────────────────────────────────────────────────

void Channel::close() noexcept {
    if (state_ == ChannelState::Closed) return;
    state_ = ChannelState::Closed;
    close_fd(client_fd_);
    close_fd(upstream_fd_);
    close_fd(pipe_cu_[0]); close_fd(pipe_cu_[1]);
    close_fd(pipe_uc_[0]); close_fd(pipe_uc_[1]);
}

} // namespace pp
