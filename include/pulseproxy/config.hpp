#pragma once
// pulseproxy/config.hpp — runtime configuration, parsed from CLI flags.
//
// All fields carry a sensible default so the binary works with
//   ./pulseproxy --upstream-host 127.0.0.1 --upstream-port 8081

#include <cstdint>
#include <string>

namespace pp {

struct Config {
    // ── Listen side ───────────────────────────────────────────────────────
    std::string listen_host = "0.0.0.0";  // bind address
    uint16_t    listen_port = 8080;        // port PulseProxy accepts on

    // ── Upstream side ─────────────────────────────────────────────────────
    std::string upstream_host = "127.0.0.1"; // target backend host
    uint16_t    upstream_port = 8081;         // target backend port

    // ── Concurrency ───────────────────────────────────────────────────────
    // 0 → use std::thread::hardware_concurrency()
    int worker_threads = 0;

    // ── Tuning ────────────────────────────────────────────────────────────
    // Maximum connections across all workers before we stop accept()ing.
    int max_connections = 100'000;

    // If true: each worker thread binds its own listener via SO_REUSEPORT.
    // The kernel distributes incoming SYNs across all listeners with no
    // mutex in user space.  Preferred when worker_threads > 1.
    bool reuseport = true;

    // Idle connection timeout in seconds (0 = disabled).
    // Connections with no read/write activity for longer than this are reclaimed.
    int idle_timeout_secs = 60;

    // Print a one-line stat to stdout every stats_interval_ms milliseconds.
    // 0 = disabled.
    int stats_interval_ms = 5000;

    // ── Parsing ───────────────────────────────────────────────────────────
    // Returns false and prints usage if args are invalid.
    static bool parse(int argc, char** argv, Config& out);
    static void print_usage(const char* argv0);
};

} // namespace pp
