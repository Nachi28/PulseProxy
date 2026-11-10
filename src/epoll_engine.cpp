// pulseproxy/epoll_engine.cpp — event loop implementation.

#include "pulseproxy/epoll_engine.hpp"
#include "pulseproxy/socket_utils.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace pp {

// Tag bits encoded in epoll_event.data.ptr to distinguish fd types.
// We pack: pointer (48 bits) | tag (3 bits low).
// All Channel* allocations are at least 8-byte aligned, so the low 3 bits
// are always zero — safe to use as a tag.
static constexpr uintptr_t TAG_LISTEN  = 1;
static constexpr uintptr_t TAG_STOP    = 2;
static constexpr uintptr_t TAG_CHANNEL = 0; // default: pointer to Channel

static void* encode_ptr(void* p, uintptr_t tag) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(p) | tag);
}
static uintptr_t decode_tag(void* p) {
    return reinterpret_cast<uintptr_t>(p) & 0x7u;
}
static Channel* decode_channel(void* p) {
    return reinterpret_cast<Channel*>(reinterpret_cast<uintptr_t>(p) & ~uintptr_t(0xFu));
}

// ── Construction / Destruction ────────────────────────────────────────────────

EpollEngine::EpollEngine(const Config& cfg, int worker_id, Fd shared_listener_fd)
    : cfg_(cfg), worker_id_(worker_id), arena_(ARENA_POOL_CAPACITY)
{
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) throw std::system_error(errno, std::system_category(), "epoll_create1");

    // eventfd — stop() writes 1, epoll_wait() wakes up.
    stop_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (stop_fd_ < 0) throw std::system_error(errno, std::system_category(), "eventfd");
    epoll_add(stop_fd_, EPOLLIN, encode_ptr(nullptr, TAG_STOP));

    // Listener socket.
    if (shared_listener_fd != INVALID_FD) {
        listen_fd_    = shared_listener_fd;
        owns_listener_ = false;
    } else {
        listen_fd_    = create_listener(cfg_.listen_host, cfg_.listen_port, cfg_.reuseport);
        owns_listener_ = true;
    }
    epoll_add(listen_fd_, EPOLLIN, encode_ptr(nullptr, TAG_LISTEN));

    std::fprintf(stderr, "[worker %d] ready on %s:%d → %s:%d\n",
                 worker_id_, cfg_.listen_host.c_str(), cfg_.listen_port,
                 cfg_.upstream_host.c_str(), cfg_.upstream_port);
}

EpollEngine::~EpollEngine() {
    for (Channel* ch : active_channels_) delete ch;
    active_channels_.clear();
    fd_table_.clear();
    close_fd(epoll_fd_);
    close_fd(stop_fd_);
    if (owns_listener_) close_fd(listen_fd_);
}

// ── Run loop ──────────────────────────────────────────────────────────────────

void EpollEngine::run() {
    std::array<epoll_event, EPOLL_MAX_EVENTS> events{};

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Wake at least once per second if idle timeouts are active
        int timeout_ms = 1000;
        if (cfg_.stats_interval_ms > 0 && cfg_.stats_interval_ms < timeout_ms) {
            timeout_ms = cfg_.stats_interval_ms;
        }

        int n = ::epoll_wait(epoll_fd_, events.data(), EPOLL_MAX_EVENTS, timeout_ms);

        if (n < 0) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "[worker %d] epoll_wait error: %s\n",
                         worker_id_, ::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i)
            dispatch_event(events[i]);

        // Periodic maintenance: sweep idle connections and log stats
        auto now = std::chrono::steady_clock::now();
        if (cfg_.idle_timeout_secs > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_sweep_).count() >= 5) {
            sweep_idle_channels();
            last_sweep_ = now;
        }

        // Print periodic stats
        if (n == 0 && cfg_.stats_interval_ms > 0) {
            std::fprintf(stderr, "[worker %d] conns active=%lu total=%lu bytes=%lu\n",
                         worker_id_,
                         connections_active(), connections_total(), bytes_proxied());
        }
    }
    std::fprintf(stderr, "[worker %d] exiting\n", worker_id_);
}

void EpollEngine::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    uint64_t val = 1;
    ::write(stop_fd_, &val, sizeof(val)); // wake epoll_wait()
}

// ── Event dispatch ────────────────────────────────────────────────────────────

void EpollEngine::dispatch_event(const epoll_event& ev) {
    uintptr_t tag = decode_tag(ev.data.ptr);

    if (tag == TAG_STOP) {
        stop_requested_.store(true, std::memory_order_relaxed);
        return;
    }

    if (tag == TAG_LISTEN) {
        accept_new_connections();
        return;
    }

    // tag == TAG_CHANNEL
    Channel* ch = decode_channel(ev.data.ptr);
    if (!ch) return;

    bool ok = true;

    if (ev.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        ch->close();
        ok = false;
    } else {
        // Direction bit encoded in pointer tag: bit 3 = 1 for upstream, 0 for client.
        bool is_upstream = (reinterpret_cast<uintptr_t>(ev.data.ptr) & 0x8u) != 0;

        if (is_upstream) {
            if (ev.events & EPOLLOUT) ok = ch->on_upstream_writable(arena_);
            if (ok && (ev.events & EPOLLIN))  ok = ch->on_upstream_readable(arena_);
        } else {
            if (ev.events & EPOLLOUT) ok = ch->on_client_writable(arena_);
            if (ok && (ev.events & EPOLLIN))  ok = ch->on_client_readable(arena_);
        }
    }

    if (!ok || ch->is_closed()) {
        deregister_and_free(ch);
    }
}

// ── Accept ────────────────────────────────────────────────────────────────────

void EpollEngine::accept_new_connections() {
    // Edge-triggered: drain the accept queue completely.
    for (;;) {
        sockaddr_in peer{};
        Fd client_fd = accept_nonblocking(listen_fd_, peer);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // queue empty
            std::fprintf(stderr, "[worker %d] accept4 error: %s\n",
                         worker_id_, ::strerror(errno));
            break;
        }

        if (conns_active_.load(std::memory_order_relaxed) >= static_cast<uint64_t>(cfg_.max_connections)) {
            std::fprintf(stderr, "[worker %d] max_connections reached, dropping\n", worker_id_);
            ::close(client_fd);
            continue;
        }

        set_tcp_nodelay(client_fd);

        Fd upstream_fd;
        try {
            upstream_fd = connect_nonblocking(cfg_.upstream_host, cfg_.upstream_port);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[worker %d] upstream connect error: %s\n",
                         worker_id_, e.what());
            ::close(client_fd);
            continue;
        }

        Fd pipe_cu[2], pipe_uc[2];
        create_pipe(pipe_cu);
        create_pipe(pipe_uc);

        Channel* ch = new Channel(client_fd, peer, upstream_fd, pipe_cu, pipe_uc);
        register_channel(ch);

        conns_total_.fetch_add(1, std::memory_order_relaxed);
        conns_active_.fetch_add(1, std::memory_order_relaxed);
    }
}

// ── Channel registration ──────────────────────────────────────────────────────

void EpollEngine::register_channel(Channel* ch) {
    // client_fd: listen for reads + errors.  Tag bit 3 = 0 (client side).
    void* client_ptr = encode_ptr(ch, TAG_CHANNEL);
    epoll_add(ch->client_fd(),
              EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET,
              client_ptr);

    // upstream_fd: listen for EPOLLOUT (connect completion) + reads.
    // Tag bit 3 = 1 to signal "upstream side".
    void* up_ptr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ch) | 0x8u);
    epoll_add(ch->upstream_fd(),
              EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP | EPOLLET,
              up_ptr);

    int max_fd = std::max(ch->client_fd(), ch->upstream_fd());
    if (static_cast<std::size_t>(max_fd) >= fd_table_.size()) {
        fd_table_.resize(static_cast<std::size_t>(max_fd + 64), nullptr);
    }
    fd_table_[ch->client_fd()]   = ch;
    fd_table_[ch->upstream_fd()] = ch;
    active_channels_.push_back(ch);
}

void EpollEngine::deregister_and_free(Channel* ch) {
    epoll_del(ch->client_fd());
    epoll_del(ch->upstream_fd());

    if (static_cast<std::size_t>(ch->client_fd()) < fd_table_.size())
        fd_table_[ch->client_fd()] = nullptr;
    if (static_cast<std::size_t>(ch->upstream_fd()) < fd_table_.size())
        fd_table_[ch->upstream_fd()] = nullptr;

    auto it = std::find(active_channels_.begin(), active_channels_.end(), ch);
    if (it != active_channels_.end()) {
        std::iter_swap(it, active_channels_.end() - 1);
        active_channels_.pop_back();
    }

    bytes_proxied_.fetch_add(ch->bytes_to_client() + ch->bytes_to_upstream(),
                             std::memory_order_relaxed);
    conns_active_.fetch_sub(1, std::memory_order_relaxed);
    delete ch;
}

void EpollEngine::sweep_idle_channels() {
    if (cfg_.idle_timeout_secs <= 0 || active_channels_.empty()) return;

    std::vector<Channel*> to_reclaim;
    for (Channel* ch : active_channels_) {
        if (ch && ch->is_idle_longer_than(cfg_.idle_timeout_secs)) {
            to_reclaim.push_back(ch);
        }
    }

    for (Channel* ch : to_reclaim) {
        std::fprintf(stderr, "[worker %d] idle timeout (> %ds inactive) for %s — reclaiming\n",
                     worker_id_, cfg_.idle_timeout_secs, ch->peer_str().c_str());
        deregister_and_free(ch);
    }
}

// ── epoll wrappers ────────────────────────────────────────────────────────────

void EpollEngine::epoll_add(Fd fd, uint32_t events, void* ptr) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = ptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::system_error(errno, std::system_category(), "epoll_ctl(ADD)");
}

void EpollEngine::epoll_mod(Fd fd, uint32_t events, void* ptr) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = ptr;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
        throw std::system_error(errno, std::system_category(), "epoll_ctl(MOD)");
}

void EpollEngine::epoll_del(Fd fd) noexcept {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
}

} // namespace pp
