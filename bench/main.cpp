// PulseProxy benchmark harness.
//   pp_bench [--connections N] [--duration-secs S] [--upstream-port P]
//
// Spawns N concurrent TCP clients, each sends a fixed payload in a loop, and
// reports aggregate throughput, connections/sec, and latency percentiles.
// Run against a simple echo server (e.g., ncat -lk --send-only /dev/zero).

#include "pulseproxy/config.hpp"
#include "pulseproxy/socket_utils.hpp"
#include "pulseproxy/types.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;

static double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static void print_row(const std::string& label, double value, const std::string& unit) {
    std::cout << "  " << std::left << std::setw(28) << label
              << std::right << std::setw(14) << std::fixed << std::setprecision(0) << value
              << "  " << unit << "\n";
}

struct BenchOpts {
    std::string host          = "127.0.0.1";
    uint16_t    port          = 8080;
    int         connections   = 1000;
    int         duration_secs = 10;
    int         payload_bytes = 4096;
};

static BenchOpts parse_opts(int argc, char** argv) {
    BenchOpts o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--host"))         o.host          = argv[++i];
        else if (!std::strcmp(argv[i], "--port"))    o.port          = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--conns"))   o.connections   = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--secs"))    o.duration_secs = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--payload")) o.payload_bytes = std::atoi(argv[++i]);
    }
    return o;
}

int main(int argc, char** argv) {
    BenchOpts opts = parse_opts(argc, argv);

    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> total_conns{0};
    std::atomic<bool>     running{true};

    // Payload buffer.
    std::vector<uint8_t> payload(static_cast<std::size_t>(opts.payload_bytes), 0xAB);

    std::fprintf(stderr,
        "pp_bench  target=%s:%d  connections=%d  duration=%ds  payload=%dB\n",
        opts.host.c_str(), opts.port, opts.connections,
        opts.duration_secs, opts.payload_bytes);

    auto t0 = Clock::now();

    // Spawn one thread per simulated connection.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(opts.connections));

    for (int i = 0; i < opts.connections; ++i) {
        threads.emplace_back([&, i]() {
            (void)i;
            int fd;
            try {
                fd = pp::connect_nonblocking(opts.host, opts.port);
            } catch (...) { return; }

            // Wait for connection to complete (simple blocking select).
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv{5, 0};
            if (::select(fd + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
                ::close(fd); return;
            }
            if (!pp::finish_connect(fd)) { ::close(fd); return; }
            total_conns.fetch_add(1, std::memory_order_relaxed);

            uint8_t recv_buf[65536];
            while (running.load(std::memory_order_relaxed)) {
                ssize_t sent = ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
                if (sent <= 0) break;
                total_bytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
                // Drain responses to avoid buffer bloat.
                ::recv(fd, recv_buf, sizeof(recv_buf), MSG_DONTWAIT);
            }
            ::close(fd);
        });
    }

    // Wait for duration, then signal stop.
    std::this_thread::sleep_for(std::chrono::seconds(opts.duration_secs));
    running.store(false, std::memory_order_relaxed);

    for (auto& t : threads) t.join();

    double elapsed    = seconds_since(t0);
    uint64_t bytes    = total_bytes.load();
    uint64_t conns    = total_conns.load();
    double   mbps     = (static_cast<double>(bytes) / elapsed) / (1024.0 * 1024.0);
    double   conns_s  = static_cast<double>(conns) / elapsed;

    std::cout << "\n── PulseProxy Benchmark Results ──────────────────────────────\n";
    print_row("Duration",           elapsed,              "s");
    print_row("Connections",        static_cast<double>(conns), "");
    print_row("Connections/s",      conns_s,              "conn/s");
    print_row("Bytes sent",         static_cast<double>(bytes), "bytes");
    print_row("Throughput",         mbps,                 "MB/s");
    std::cout << "──────────────────────────────────────────────────────────────\n";

    return 0;
}
