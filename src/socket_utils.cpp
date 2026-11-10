// pulseproxy/socket_utils.cpp
#include "pulseproxy/socket_utils.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace pp {

// ── Internal helpers ─────────────────────────────────────────────────────────

static void throw_errno(const char* what) {
    throw std::system_error(errno, std::system_category(), what);
}

static void setsockopt_int(Fd fd, int level, int opt, int val, const char* name) {
    if (::setsockopt(fd, level, opt, &val, sizeof(val)) < 0)
        throw_errno(name);
}

// ── Non-blocking flag ─────────────────────────────────────────────────────────

void set_nonblocking(Fd fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) throw_errno("fcntl(F_GETFL)");
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw_errno("fcntl(F_SETFL, O_NONBLOCK)");
}

// ── Listener socket ───────────────────────────────────────────────────────────

Fd create_listener(const std::string& host, uint16_t port, bool reuseport) {
    Fd fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw_errno("socket(listener)");

    setsockopt_int(fd, SOL_SOCKET, SO_REUSEADDR, 1, "SO_REUSEADDR");
    if (reuseport)
        setsockopt_int(fd, SOL_SOCKET, SO_REUSEPORT, 1, "SO_REUSEPORT");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid listen host: " + host);
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        throw_errno("bind");
    }
    if (::listen(fd, LISTEN_BACKLOG) < 0) {
        ::close(fd);
        throw_errno("listen");
    }
    return fd;
}

// ── Upstream connect ──────────────────────────────────────────────────────────

Fd connect_nonblocking(const std::string& host, uint16_t port) {
    Fd fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) throw_errno("socket(upstream)");

    set_tcp_nodelay(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid upstream host: " + host);
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        throw_errno("connect");
    }
    // errno == EINPROGRESS → caller waits for EPOLLOUT before calling finish_connect()
    return fd;
}

bool finish_connect(Fd fd) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return false;
    if (err != 0) { errno = err; return false; }
    return true;
}

// ── Accept ────────────────────────────────────────────────────────────────────

Fd accept_nonblocking(Fd listen_fd, sockaddr_in& peer_addr) {
    socklen_t len = sizeof(peer_addr);
    // accept4 sets SOCK_NONBLOCK and SOCK_CLOEXEC atomically — no extra fcntl.
    Fd fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &len,
                      SOCK_NONBLOCK | SOCK_CLOEXEC);
    // EAGAIN / EWOULDBLOCK: accept queue drained — normal, not an error.
    return fd; // caller checks fd < 0 && errno == EAGAIN
}

// ── Socket options ────────────────────────────────────────────────────────────

void set_tcp_nodelay(Fd fd) {
    setsockopt_int(fd, IPPROTO_TCP, TCP_NODELAY, 1, "TCP_NODELAY");
}

void set_keepalive(Fd fd) {
    setsockopt_int(fd, SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE");
}

// ── Pipe helpers ──────────────────────────────────────────────────────────────

void create_pipe(Fd out[2]) {
    // O_NONBLOCK on both ends: splice() returns EAGAIN when the pipe is full
    // rather than blocking the event loop.
    if (::pipe2(out, O_NONBLOCK | O_CLOEXEC) < 0)
        throw_errno("pipe2");
}

// ── Close ─────────────────────────────────────────────────────────────────────

void close_fd(Fd& fd) noexcept {
    if (fd != INVALID_FD) {
        ::close(fd);
        fd = INVALID_FD;
    }
}

} // namespace pp
