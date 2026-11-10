// pulseproxy/config.cpp — CLI argument parsing.
#include "pulseproxy/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace pp {

void Config::print_usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --listen-host HOST      Bind address            (default: 0.0.0.0)\n"
        "  --listen-port PORT      Listen port             (default: 8080)\n"
        "  --upstream-host HOST    Backend host            (default: 127.0.0.1)\n"
        "  --upstream-port PORT    Backend port            (default: 8081)\n"
        "  --workers N             Worker threads          (default: nproc)\n"
        "  --max-connections N     Connection cap          (default: 100000)\n"
        "  --no-reuseport          Disable SO_REUSEPORT\n"
        "  --timeout-secs SECS     Idle connection timeout (default: 60, 0=off)\n"
        "  --stats-interval MS     Stats period ms (0=off) (default: 5000)\n"
        "  --help                  Show this message\n",
        argv0);
}

bool Config::parse(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", arg);
                std::exit(1);
            }
            return argv[++i];
        };

        if      (!std::strcmp(arg, "--listen-host"))     cfg.listen_host    = next();
        else if (!std::strcmp(arg, "--listen-port"))     cfg.listen_port    = static_cast<uint16_t>(std::atoi(next()));
        else if (!std::strcmp(arg, "--upstream-host"))   cfg.upstream_host  = next();
        else if (!std::strcmp(arg, "--upstream-port"))   cfg.upstream_port  = static_cast<uint16_t>(std::atoi(next()));
        else if (!std::strcmp(arg, "--workers"))         cfg.worker_threads = std::atoi(next());
        else if (!std::strcmp(arg, "--max-connections")) cfg.max_connections = std::atoi(next());
        else if (!std::strcmp(arg, "--no-reuseport"))    cfg.reuseport      = false;
        else if (!std::strcmp(arg, "--timeout-secs"))    cfg.idle_timeout_secs = std::atoi(next());
        else if (!std::strcmp(arg, "--stats-interval"))  cfg.stats_interval_ms = std::atoi(next());
        else if (!std::strcmp(arg, "--help")) { print_usage(argv[0]); return false; }
        else {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg);
            print_usage(argv[0]);
            return false;
        }
    }

    // Resolve 0 → hardware_concurrency (fall back to 1 if unavailable).
    if (cfg.worker_threads <= 0) {
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        cfg.worker_threads = (hw > 0) ? hw : 1;
    }
    return true;
}

} // namespace pp
