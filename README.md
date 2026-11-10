# PulseProxy

A high-performance asynchronous TCP reverse proxy written from scratch in C++20.
Linux `epoll` (edge-triggered), `splice()` zero-copy forwarding, and a per-thread
fixed-block memory arena — no heap allocation on the data path after startup.
No third-party dependencies.

[![ci](https://github.com/Nachi28/PulseProxy/actions/workflows/ci.yml/badge.svg)](https://github.com/Nachi28/PulseProxy/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)

```
$ ./pulseproxy --listen-port 8080 --upstream-port 8081 --workers 4
PulseProxy v1.0  listen=0.0.0.0:8080  upstream=127.0.0.1:8081  workers=4
[worker 0] ready on 0.0.0.0:8080 → 127.0.0.1:8081
[worker 1] ready on 0.0.0.0:8080 → 127.0.0.1:8081
[worker 2] ready on 0.0.0.0:8080 → 127.0.0.1:8081
[worker 3] ready on 0.0.0.0:8080 → 127.0.0.1:8081
[worker 0] conns active=12483 total=24918 bytes=1073741824
```

## Build

CMake 3.18+, any C++20 compiler, Linux kernel 3.9+ (for `SO_REUSEPORT`) — nothing else:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run

```bash
# Forward :8080 → 127.0.0.1:8081, one worker per CPU core
./build/pulseproxy --upstream-port 8081

# Single-threaded, no SO_REUSEPORT, stats every second
./build/pulseproxy --upstream-port 8081 --workers 1 --no-reuseport --stats-interval 1000

# All options
./build/pulseproxy --help
```

## Why edge-triggered `epoll` matters

A Level-Triggered (LT) `epoll` fires every iteration as long as a socket has
data waiting.  Under sustained load, this means thousands of redundant `epoll_wait()`
wakeups per second for already-known-readable sockets.

Edge-Triggered (ET) fires **once per state transition** — only when new data
arrives. The trade-off: you must drain the socket in a tight loop (reading until
`EAGAIN`), or you miss data until the next packet arrives:

```cpp
// Drain until EAGAIN — the kernel won't notify again until more data arrives.
for (;;) {
    ssize_t n = splice(client_fd, nullptr, pipe_write, nullptr,
                       ARENA_BLOCK_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
    if (n <= 0) break;  // 0 = EOF, -1 = EAGAIN — stop looping
    bytes_forwarded += n;
}
```

The result: at 100,000 concurrent connections, `epoll_wait()` wakes up only
when there is real work to do — never to re-check a socket it already knows is idle.

## Zero-copy with `splice()`

Standard proxies copy bytes through user space:

```
NIC → Kernel Socket Buffer → User Buffer → Kernel Socket Buffer → NIC
       ── 2 DMA transfers ──   2 memcpy   ── 2 DMA transfers ──
```

PulseProxy calls `splice()` to transfer kernel page references through an
anonymous pipe between two sockets — the CPU never touches the payload bytes:

```
NIC → Kernel Socket Buffer → Kernel Pipe → Kernel Socket Buffer → NIC
       ── 2 DMA transfers ──   0 memcpy   ── 2 DMA transfers ──
```

At 10 Gbit/s line rate, cutting two memcpy passes on 64 kB chunks saves
roughly **12 µs of CPU time per round trip** and reduces LLC cache pressure.

## `SO_REUSEPORT` multi-listener model

Each worker thread binds its own listener socket to the same port:

```
Worker 0: listen_fd_0 bound to :8080  ─┐
Worker 1: listen_fd_1 bound to :8080  ─┤─ kernel balances SYNs by 4-tuple hash
Worker 2: listen_fd_2 bound to :8080  ─┤
Worker 3: listen_fd_3 bound to :8080  ─┘
```

No shared `accept()` mutex, no thundering herd.  New connections land directly
in the epoll queue of one worker without waking the others.

## Architecture

```
CLI args
   │  Config::parse()
   v
main.cpp
   │  spawn N std::threads
   v
EpollEngine (one per thread)
   │  epoll_wait() ← EPOLLET, edge-triggered
   │
   ├── LISTEN fd  → accept4() → new Channel
   │                             ├── client_fd  (non-blocking, TCP_NODELAY)
   │                             └── upstream_fd (non-blocking, connecting)
   │
   ├── upstream fd EPOLLOUT → Channel::on_upstream_connected()
   │                               → state: Connecting → Active
   │
   ├── client fd  EPOLLIN  → Channel::on_client_readable()
   │                               → splice(client → pipe_cu → upstream)
   │
   ├── upstream fd EPOLLIN → Channel::on_upstream_readable()
   │                               → splice(upstream → pipe_uc → client)
   │
   └── EPOLLHUP / EPOLLERR → Channel::close() → deregister + free
```

## What it does

| Feature | Detail |
|---|---|
| **Protocol** | Transparent TCP proxy (any application protocol) |
| **I/O model** | `epoll(7)` edge-triggered, non-blocking throughout |
| **Data path** | `splice(2)` zero-copy, kernel-to-kernel page transfer |
| **Concurrency** | N worker threads, each with its own epoll + arena |
| **Load balancing** | `SO_REUSEPORT` — kernel-level, no mutex |
| **Memory** | Fixed-block arena pool — zero `malloc` after startup |
| **Back-pressure** | Pending pipe bytes tracked; `EPOLLOUT` re-arms drain |
| **Graceful shutdown** | `eventfd` wakes `epoll_wait()`, all channels torn down |
| **Connection limits** | `--max-connections` cap enforced per worker |
| **Idle Timeout** | Reclaims inactive connections via `--timeout-secs` |
| **Dispatch** | Direct flat-array indexing (`O(1)` zero hash overhead) |
| **Verification** | In-process E2E integration tests (5 MB streaming, RST, timeout) |
| **Stats** | Bytes proxied, active/total connections, per worker |

## Performance

`pp_bench --conns 10000 --secs 30` against a loopback echo server, 4-core VM:

| Metric | |
|---|---|
| Concurrent connections sustained | 10,000+ |
| Throughput (loopback) | ~9,200 MB/s |
| CPU per 10k active connections | ~8% (1 core) |
| Latency p50 (loopback) | < 0.3 ms |
| Latency p99 (loopback) | < 1.1 ms |

> Numbers measured inside WSL2 (Debian 12, g++ 12.2.0, kernel 5.15, 4 vCPU).

## Documentation

| | |
|---|---|
| [`docs/HANDBOOK.md`](docs/HANDBOOK.md) | Complete reference: every file, invariant, and recipes for extending |
| [`docs/DESIGN.md`](docs/DESIGN.md) | Why each decision was made, and what was rejected |
| [`examples/`](examples/) | Runnable shell scripts demonstrating each mode |

## Limitations

Deliberate, documented rather than hidden. TCP only — no TLS (the proxy is
transparent; TLS termination belongs in a separate layer). IPv4 only (IPv6 is
one `AF_INET6` swap away). No HTTP-layer awareness — this is a raw L4 proxy,
not an HTTP reverse proxy like nginx. No hot config reload. Single upstream
target per process (multiple upstreams = multiple processes). Full list in the
handbook.

## License

MIT — see [LICENSE](LICENSE).
