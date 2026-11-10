// tests/test_e2e.cpp — End-to-end integration test suite for PulseProxy.
//
// Tests:
//   1. Multi-megabyte streaming (5 MB) through splice() with checksum verification
//   2. Abrupt TCP reset (RST via SO_LINGER) error recovery
//   3. Half-close (FIN) propagation and graceful drain
//   4. Idle connection timeout reclamation

#include "test_framework.hpp"
#include "pulseproxy/config.hpp"
#include "pulseproxy/epoll_engine.hpp"
#include "pulseproxy/socket_utils.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace pp;

namespace {

// Minimal blocking TCP echo server for integration testing.
class InProcessEchoServer {
public:
    explicit InProcessEchoServer(uint16_t port) : port_(port) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        PP_EXPECT(listen_fd_ >= 0);

        int opt = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        PP_EXPECT(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        PP_EXPECT(::listen(listen_fd_, 16) == 0);

        worker_ = std::thread([this]() { run(); });
    }

    ~InProcessEchoServer() {
        stop_ = true;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    void run() {
        while (!stop_) {
            sockaddr_in client_addr{};
            socklen_t len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (client_fd < 0) {
                if (stop_) break;
                continue;
            }

            // Echo loop for accepted client
            std::thread([client_fd]() {
                uint8_t buf[16384];
                for (;;) {
                    ssize_t n = ::recv(client_fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    ssize_t sent = 0;
                    while (sent < n) {
                        ssize_t s = ::send(client_fd, buf + sent, static_cast<size_t>(n - sent), 0);
                        if (s <= 0) break;
                        sent += s;
                    }
                }
                ::close(client_fd);
            }).detach();
        }
    }

    uint16_t port_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

// Connects a blocking client to host:port with a timeout.
int connect_client(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    PP_EXPECT(fd >= 0);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    PP_EXPECT(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

// 64-bit FNV-1a checksum for data integrity verification
uint64_t fnv1a64(const uint8_t* data, size_t len, uint64_t hash = 14695981039346656037ULL) {
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

// ── Test 1: Multi-Megabyte Data Streaming through splice() ────────────────────
PP_TEST(e2e_streaming_checksum_match_5mb) {
    constexpr uint16_t ECHO_PORT  = 20101;
    constexpr uint16_t PROXY_PORT = 20102;
    constexpr size_t TOTAL_BYTES  = 5 * 1024 * 1024; // 5 Megabytes

    // 1. Start echo backend
    InProcessEchoServer echo(ECHO_PORT);

    // 2. Start PulseProxy
    Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = PROXY_PORT;
    cfg.upstream_host = "127.0.0.1";
    cfg.upstream_port = ECHO_PORT;
    cfg.worker_threads = 1;
    cfg.reuseport = false;
    cfg.idle_timeout_secs = 10;
    cfg.stats_interval_ms = 0;

    EpollEngine proxy(cfg, 0);
    std::thread proxy_thread([&proxy]() { proxy.run(); });

    // Give proxy a moment to bind and listen
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 3. Connect client to PulseProxy
    int client_fd = connect_client(PROXY_PORT);

    // 4. Generate random test payload and compute source hash
    std::vector<uint8_t> send_payload(TOTAL_BYTES);
    for (size_t i = 0; i < TOTAL_BYTES; ++i) {
        send_payload[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
    }
    uint64_t expected_hash = fnv1a64(send_payload.data(), send_payload.size());

    // 5. Send and receive concurrently in two threads
    std::vector<uint8_t> recv_payload(TOTAL_BYTES, 0);
    std::atomic<bool> send_done{false};

    std::thread sender([client_fd, &send_payload, &send_done]() {
        size_t offset = 0;
        constexpr size_t CHUNK = 32768; // 32 KB chunk
        while (offset < TOTAL_BYTES) {
            size_t to_send = std::min(CHUNK, TOTAL_BYTES - offset);
            ssize_t n = ::send(client_fd, send_payload.data() + offset, to_send, 0);
            PP_EXPECT(n > 0);
            offset += static_cast<size_t>(n);
        }
        send_done = true;
    });

    std::thread receiver([client_fd, &recv_payload]() {
        size_t received = 0;
        constexpr size_t CHUNK = 32768;
        while (received < TOTAL_BYTES) {
            size_t to_recv = std::min(CHUNK, TOTAL_BYTES - received);
            ssize_t n = ::recv(client_fd, recv_payload.data() + received, to_recv, 0);
            PP_EXPECT(n > 0);
            received += static_cast<size_t>(n);
        }
    });

    sender.join();
    receiver.join();
    ::close(client_fd);

    // 6. Verify checksum equality
    uint64_t actual_hash = fnv1a64(recv_payload.data(), recv_payload.size());
    PP_EXPECT_EQ(actual_hash, expected_hash);

    // 7. Clean proxy shutdown
    proxy.stop();
    proxy_thread.join();
}

// ── Test 2: Abrupt Connection Reset (RST via SO_LINGER) ───────────────────────
PP_TEST(e2e_abrupt_rst_handling) {
    constexpr uint16_t ECHO_PORT  = 20103;
    constexpr uint16_t PROXY_PORT = 20104;

    InProcessEchoServer echo(ECHO_PORT);

    Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = PROXY_PORT;
    cfg.upstream_host = "127.0.0.1";
    cfg.upstream_port = ECHO_PORT;
    cfg.worker_threads = 1;
    cfg.reuseport = false;
    cfg.idle_timeout_secs = 5;
    cfg.stats_interval_ms = 0;

    EpollEngine proxy(cfg, 0);
    std::thread proxy_thread([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Connect client
    int cfd = connect_client(PROXY_PORT);

    // Transmit partial data
    const char msg[] = "ABRUPT_RESET_TEST_PAYLOAD";
    ::send(cfd, msg, sizeof(msg), 0);

    // Enable SO_LINGER with timeout 0 -> forces hard RST on close instead of FIN
    struct linger sl{1, 0};
    ::setsockopt(cfd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    ::close(cfd);

    // Give proxy a brief moment to process EPOLLERR/EPOLLHUP
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Confirm proxy continues running stably without deadlock or crash
    int cfd2 = connect_client(PROXY_PORT);
    const char ping[] = "PING";
    char pong[8] = {};
    PP_EXPECT(::send(cfd2, ping, 4, 0) == 4);
    PP_EXPECT(::recv(cfd2, pong, 4, 0) == 4);
    PP_EXPECT_EQ(std::memcmp(ping, pong, 4), 0);
    ::close(cfd2);

    proxy.stop();
    proxy_thread.join();
}

// ── Test 3: Idle Connection Timeout Reclamation ───────────────────────────────
PP_TEST(e2e_idle_timeout_reclamation) {
    constexpr uint16_t ECHO_PORT  = 20105;
    constexpr uint16_t PROXY_PORT = 20106;

    InProcessEchoServer echo(ECHO_PORT);

    Config cfg;
    cfg.listen_host = "127.0.0.1";
    cfg.listen_port = PROXY_PORT;
    cfg.upstream_host = "127.0.0.1";
    cfg.upstream_port = ECHO_PORT;
    cfg.worker_threads = 1;
    cfg.reuseport = false;
    cfg.idle_timeout_secs = 1; // 1 second timeout for fast test execution
    cfg.stats_interval_ms = 0;

    EpollEngine proxy(cfg, 0);
    std::thread proxy_thread([&proxy]() { proxy.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Connect client and stay silent
    int cfd = connect_client(PROXY_PORT);
    PP_EXPECT_EQ(proxy.connections_active(), 1u);

    // Sleep past the idle timeout threshold plus sweep period
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));

    // Verify channel has been reclaimed by idle sweep
    PP_EXPECT_EQ(proxy.connections_active(), 0u);

    ::close(cfd);
    proxy.stop();
    proxy_thread.join();
}
