#pragma once
// pulseproxy/socket_utils.hpp — POSIX socket helpers.
//
// Every function here is a thin, testable wrapper around a single Linux
// system call or setsockopt().  No business logic lives here.

#include "pulseproxy/types.hpp"
#include <netinet/in.h>  // sockaddr_in
#include <string>

namespace pp {

// ── Non-blocking flag ────────────────────────────────────────────────────────

// Set O_NONBLOCK on fd.  Throws std::system_error on failure.
void set_nonblocking(Fd fd);

// ── Listener socket ──────────────────────────────────────────────────────────

// Create a TCP socket that is ready to accept connections on host:port.
// Options applied:
//   SO_REUSEADDR  — always
//   SO_REUSEPORT  — when reuseport == true (required for multi-worker binding)
//   O_NONBLOCK    — always (epoll edge-triggered requires it)
//
// Returns the listening file descriptor.  Throws std::system_error on failure.
Fd create_listener(const std::string& host, uint16_t port, bool reuseport = true);

// ── Client-side connection to upstream ───────────────────────────────────────

// Open a non-blocking TCP socket pointed at host:port and issue connect().
// Because the socket is non-blocking, connect() will almost always return
// EINPROGRESS — the caller must wait for EPOLLOUT (writable) to confirm.
//
// Returns the socket fd.  Throws std::system_error on hard failure.
Fd connect_nonblocking(const std::string& host, uint16_t port);

// Check whether a pending non-blocking connect() succeeded.
// Call after EPOLLOUT fires on the upstream fd.
// Returns true on success, false and sets errno on connection refused / reset.
bool finish_connect(Fd fd);

// ── Accept ───────────────────────────────────────────────────────────────────

// Wraps accept4(SOCK_NONBLOCK | SOCK_CLOEXEC).
// Returns INVALID_FD (and errno == EAGAIN) when the accept queue is drained —
// caller should stop looping and re-arm epoll.
Fd accept_nonblocking(Fd listen_fd, sockaddr_in& peer_addr);

// ── Socket options ───────────────────────────────────────────────────────────

// Disable Nagle — mandatory for latency-sensitive proxy paths.
void set_tcp_nodelay(Fd fd);

// Set SO_KEEPALIVE so the OS probes idle connections instead of leaking them.
void set_keepalive(Fd fd);

// ── Pipe helpers for splice() ────────────────────────────────────────────────

// Create an anonymous kernel pipe with O_NONBLOCK on both ends.
// out[0] = read end, out[1] = write end.
void create_pipe(Fd out[2]);

// ── Utility ──────────────────────────────────────────────────────────────────

// Close fd and set it to INVALID_FD.  No-op if already INVALID_FD.
void close_fd(Fd& fd) noexcept;

} // namespace pp
