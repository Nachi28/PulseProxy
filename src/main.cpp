// pulseproxy — entry point.
//
// Spawns N worker threads each running an EpollEngine, then blocks until
// SIGINT / SIGTERM.  On signal, calls stop() on every engine and joins.

#include "pulseproxy/config.hpp"
#include "pulseproxy/epoll_engine.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

// Global list of all engines — populated before SIGINT handler is registered.
std::vector<pp::EpollEngine*> g_engines;

void signal_handler(int /*sig*/) {
    for (auto* e : g_engines) e->stop();
}

} // namespace

int main(int argc, char** argv) {
    pp::Config cfg;
    if (!pp::Config::parse(argc, argv, cfg)) return 1;

    std::fprintf(stderr,
        "PulseProxy v1.0  listen=%s:%d  upstream=%s:%d  workers=%d\n",
        cfg.listen_host.c_str(), cfg.listen_port,
        cfg.upstream_host.c_str(), cfg.upstream_port,
        cfg.worker_threads);

    // Build engines.
    std::vector<std::unique_ptr<pp::EpollEngine>> engines;
    engines.reserve(static_cast<std::size_t>(cfg.worker_threads));
    for (int i = 0; i < cfg.worker_threads; ++i)
        engines.push_back(std::make_unique<pp::EpollEngine>(cfg, i));

    // Populate global pointer list for signal handler.
    for (auto& e : engines) g_engines.push_back(e.get());

    // Install signal handler.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    ::sigaction(SIGINT,  &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    // Spawn worker threads.
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(cfg.worker_threads));
    for (auto& e : engines)
        threads.emplace_back([&e]{ e->run(); });

    // Wait for all workers to finish.
    for (auto& t : threads) t.join();

    // Print final aggregate stats.
    uint64_t total_conns  = 0;
    uint64_t total_bytes  = 0;
    for (const auto& e : engines) {
        total_conns += e->connections_total();
        total_bytes += e->bytes_proxied();
    }
    std::fprintf(stderr,
        "\nShutdown complete. connections=%lu  bytes_proxied=%lu\n",
        total_conns, total_bytes);

    return 0;
}
