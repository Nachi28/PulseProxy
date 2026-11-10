# PulseProxy Handbook

Technical reference for the PulseProxy project. Written for:

- **Any engineer** needing to understand the full system before making changes.
- **A maintainer** revisiting the codebase months later who needs the exact
  invariants, Linux syscall constraints, and byte layouts.

Everything needed to work on PulseProxy is in this document. Where a claim is
measurable, the command that measures it is given. Where a decision could have
gone another way, the rejected alternative is in `docs/DESIGN.md`.

Companion documents: `README.md` is the shop window (what it does, how to run
it, with numbers). `docs/DESIGN.md` is the decision log (why each choice was
made). This handbook is the reference (what exists and how it fits together).
When they disagree, the code wins — and then this file should be corrected.

---

## Contents

| | Section | Read it for |
|---|---|---|
| 1 | What PulseProxy is | Scope, what it is not |
| 2 | Quick start | Build, run, verify, WSL2 specifics |
| 3 | Repository map | Every file and its job |
| 4 | Architecture | Layer stack, ownership, event flow |
| 5 | Module reference | API and behaviour of each component |
| 6 | The event loop in detail | epoll lifecycle, edge-triggered drain |
| 7 | The data path in detail | splice() mechanics, back-pressure |
| 8 | Invariants | The rules that must hold |
| 9 | Testing | Suites, how to run, what each tests |
| 10 | Performance | Numbers, method, capacity arithmetic |
| 11 | Known limitations | Deliberate gaps, where a fix would go |
| 12 | How to extend it | Recipes for likely changes |
| 13 | Working on this code | Conventions and traps |
| 14 | Glossary | Terms as used here |

---

## 1. What PulseProxy is

A **transparent TCP reverse proxy** written from scratch in C++20 with no
third-party dependencies.  It accepts connections on a listen port, opens a
matching connection to a fixed upstream host:port, and bidirectionally forwards
all bytes with zero user-space copies.

It demonstrates four core systems programming techniques:

| Technique | Implementation |
|---|---|
| Linux async I/O | `epoll(7)` with `EPOLLET` (edge-triggered) |
| Zero-copy forwarding | `splice(2)` via kernel pipe intermediary |
| Multi-core scalability | `SO_REUSEPORT` multi-listener, one epoll per thread |
| Allocation-free hot path | Fixed-block `MemoryArena` per worker thread |

**Status: core proxy engine complete.** TLS termination, HTTP-layer parsing, and
multi-upstream load balancing are out of scope and documented as deliberate
limitations.

---

## 2. Quick start

### WSL2 / Linux

```bash
# Prerequisites: g++ 12+, cmake 3.18+, pthreads (standard), ncat (for examples)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Run the echo proxy demo
bash examples/echo_proxy.sh
```

### WSL2 specifics

PulseProxy is developed and tested on Windows 11 with WSL2 (Debian 12,
kernel 5.15, g++ 12.2.0).  All Linux-specific syscalls (`epoll_create1`,
`splice`, `accept4`, `eventfd`, `SO_REUSEPORT`) are natively available inside
WSL2 — no emulation layer.

The binary only runs on Linux.  It does not build on Windows natively (no
`<sys/epoll.h>`).  CI runs on `ubuntu-latest`.

### Adjust `ulimit` for high connection counts

Each channel uses 4 file descriptors (client_fd, upstream_fd, 2 × pipe).
For 100,000 concurrent connections: 400,000 fds.

```bash
ulimit -n 500000
# Or persistently: edit /etc/security/limits.conf
```

---

## 3. Repository map

```
PulseProxy/
├── CMakeLists.txt           # root build, defines pp_core + pulseproxy + pp_bench
├── LICENSE
├── README.md                # feature overview, quick-start, benchmark numbers
│
├── include/pulseproxy/
│   ├── types.hpp            # Fd alias, constants, ChannelState enum
│   ├── config.hpp           # Config struct + Config::parse()
│   ├── socket_utils.hpp     # thin POSIX wrappers (create_listener, splice pipe, …)
│   ├── memory_arena.hpp     # fixed-block buffer pool, per-thread, zero-alloc
│   ├── channel.hpp          # one client↔upstream connection pair + state machine
│   └── epoll_engine.hpp     # event loop: epoll_wait dispatch, accept, lifecycle
│
├── src/
│   ├── config.cpp           # CLI parser (--listen-port, --upstream-port, …)
│   ├── socket_utils.cpp     # POSIX implementation
│   ├── memory_arena.cpp     # arena LIFO stack implementation
│   ├── channel.cpp          # splice() forward, state transitions, teardown
│   ├── epoll_engine.cpp     # epoll_create1, event loop, accept4, channel dispatch
│   └── main.cpp             # spawn threads, signal handler, aggregate stats
│
├── tests/
│   ├── CMakeLists.txt
│   ├── test_framework.hpp   # PP_TEST, PP_EXPECT macros — zero deps
│   ├── test_main.cpp        # runner entry point
│   ├── test_arena.cpp       # MemoryArena unit tests
│   ├── test_socket_utils.cpp# socket helper unit tests (loopback)
│   └── test_config.cpp      # Config parsing unit tests
│
├── bench/
│   └── main.cpp             # pp_bench: N concurrent clients, throughput + latency
│
├── examples/
│   └── echo_proxy.sh        # end-to-end demo (ncat echo backend)
│
└── docs/
    ├── HANDBOOK.md          # ← this file
    └── DESIGN.md            # decision log
```

### Build targets

| Target | Binary | Description |
|---|---|---|
| `pp_core` | (library) | All proxy logic; linked by the other three |
| `pulseproxy` | `build/pulseproxy` | The proxy server |
| `pp_bench` | `build/pp_bench` | Load-generator / benchmark client |
| `pp_tests` | `build/tests/pp_tests` | Unit test runner |

---

## 4. Architecture

### Layer stack

```
main.cpp
  │  parse Config
  │  spawn N std::threads, each running EpollEngine::run()
  │  block on thread::join()
  │
  └─► EpollEngine (one per thread, one per CPU core)
        │
        │  epoll_wait(EPOLLET) ← blocks until events arrive
        │
        ├── listen_fd  EPOLLIN  → accept_new_connections()
        │                           accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)
        │                           connect_nonblocking(upstream)
        │                           new Channel(client_fd, upstream_fd, pipes)
        │                           epoll_add(client_fd | upstream_fd, EPOLLET)
        │
        ├── upstream_fd EPOLLOUT (Connecting state)
        │                       → Channel::on_upstream_connected()
        │                           getsockopt(SO_ERROR) → check success
        │                           state: Connecting → Active
        │
        ├── client_fd  EPOLLIN  → Channel::on_client_readable()
        │                           splice(client_fd → pipe_cu → upstream_fd)
        │                           loop until EAGAIN / 0 (EOF)
        │
        ├── upstream_fd EPOLLIN → Channel::on_upstream_readable()
        │                           splice(upstream_fd → pipe_uc → client_fd)
        │                           loop until EAGAIN / 0 (EOF)
        │
        ├── EPOLLOUT (Active)   → on_client_writable() / on_upstream_writable()
        │                           drain backpressured pipe bytes
        │
        └── EPOLLERR / EPOLLHUP → Channel::close() → deregister + delete
```

### Ownership

```
EpollEngine owns:
  epoll_fd_        — one epoll instance
  listen_fd_       — one listener (or shared)
  stop_fd_         — eventfd for clean shutdown
  fd_table_        — direct-indexed vector for O(1) FD lookup
  active_channels_ — active Channel list for lifecycle and idle sweeps
  arena_           — MemoryArena for this thread

Channel owns:
  client_fd_      — the accepted client socket
  upstream_fd_    — the upstream backend socket
  pipe_cu_[2]     — kernel pipe: client → upstream
  pipe_uc_[2]     — kernel pipe: upstream → client
```

No object is shared between workers except `Config` (read-only after `parse()`).

### Error model

Every syscall failure that is not `EAGAIN`/`EINPROGRESS` results in either:
- `throw std::system_error(errno, ...)` during setup (fatal — the process exits).
- `channel.close()` during operation (non-fatal — the channel is cleaned up,
  the worker continues).

`EAGAIN` on reads/writes is never an error.  It is the signal that the socket
buffer is drained (reads) or full (writes) — both are handled with
back-pressure.

---

## 5. Module reference

### `types.hpp`

Defines the shared vocabulary:

| Symbol | Type | Value | Meaning |
|---|---|---|---|
| `Fd` | `int` | — | File descriptor alias |
| `INVALID_FD` | `Fd` | `-1` | Sentinel for unset/closed fd |
| `EPOLL_MAX_EVENTS` | `int` | `128` | Events per `epoll_wait()` call |
| `ARENA_BLOCK_SIZE` | `size_t` | `65536` | One arena block = 64 kB |
| `ARENA_POOL_CAPACITY` | `size_t` | `256` | Blocks per worker arena = 16 MB |
| `LISTEN_BACKLOG` | `int` | `SOMAXCONN` | Kernel SYN queue depth |
| `ChannelState` | enum | — | `Connecting`, `Active`, `HalfClosed`, `Closed` |
| `Direction` | enum | — | `ClientToUpstream`, `UpstreamToClient` |

### `config.hpp` / `config.cpp`

`Config` is a plain struct with public fields.  `Config::parse(argc, argv, out)`
fills it from CLI flags, resolves `worker_threads = 0` to
`hardware_concurrency()`, and returns `false` on parse error.

The struct is constructed once in `main()` and passed by const-reference to
every `EpollEngine`.  No mutex needed: the struct is not mutated after parsing.

### `socket_utils.hpp` / `socket_utils.cpp`

One function per syscall.  Each throws `std::system_error` on hard failure.
Returns `INVALID_FD` (not an exception) for `accept_nonblocking()` returning
`EAGAIN` — that is not an error.

| Function | Wraps | Note |
|---|---|---|
| `set_nonblocking(fd)` | `fcntl(F_SETFL, O_NONBLOCK)` | |
| `create_listener(host, port, reuseport)` | `socket + bind + listen` | always `O_NONBLOCK` |
| `connect_nonblocking(host, port)` | `socket + connect` | returns on `EINPROGRESS` |
| `finish_connect(fd)` | `getsockopt(SO_ERROR)` | call after `EPOLLOUT` |
| `accept_nonblocking(listen_fd, peer)` | `accept4` | `SOCK_NONBLOCK\|SOCK_CLOEXEC` |
| `set_tcp_nodelay(fd)` | `setsockopt(TCP_NODELAY)` | |
| `set_keepalive(fd)` | `setsockopt(SO_KEEPALIVE)` | |
| `create_pipe(out[2])` | `pipe2(O_NONBLOCK\|O_CLOEXEC)` | |
| `close_fd(fd)` | `close` | sets fd to `INVALID_FD` after |

### `memory_arena.hpp` / `memory_arena.cpp`

Pre-allocates `capacity` blocks of `ARENA_BLOCK_SIZE` bytes in one contiguous
allocation.  Maintains a LIFO stack of free pointers.

- `acquire()` → `uint8_t*` or `nullptr` if exhausted.
- `release(ptr)` → returns block to the free-list.

Both operations are `O(1)` and involve no system calls after construction.
Not thread-safe by design — one arena per worker.

Each `Block` is declared `alignas(64)` to place it on a 64-byte cache-line
boundary.  Adjacent blocks in the pool will not share a cache line, preventing
false sharing if two threads ever access neighbouring blocks (which they cannot
in this design, but the alignment is cheap and correct).

### `channel.hpp` / `channel.cpp`

**State machine:**

```
Connecting  ──(on_upstream_connected success)──► Active
Connecting  ──(on_upstream_connected failure)──► Closed
Active      ──(client EOF)─────────────────────► HalfClosed
Active      ──(upstream EOF, client still open)─► HalfClosed
HalfClosed  ──(other side EOF)─────────────────► Closed
Any         ──(EPOLLERR / EPOLLHUP)────────────► Closed
```

**Data path (client → upstream):**

```cpp
// splice: client socket buffer → pipe write-end (kernel, zero copy)
ssize_t in = splice(client_fd, nullptr, pipe_cu[1], nullptr,
                    ARENA_BLOCK_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);

// splice: pipe read-end → upstream socket buffer (kernel, zero copy)
ssize_t out = splice(pipe_cu[0], nullptr, upstream_fd, nullptr,
                     in, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
```

If `out` returns `EAGAIN`: bytes remain in `pipe_cu`.  The engine arms
`EPOLLOUT` on `upstream_fd`.  When `upstream_fd` becomes writable,
`on_upstream_writable()` drains the pipe by calling `on_client_readable()` again.

**Half-close semantics:**
When the client sends FIN (splice returns 0), `shutdown(upstream_fd, SHUT_WR)`
is called.  This forwards the FIN to the backend.  The upstream side may still
send data; the channel stays alive in `HalfClosed` state until both sides are
done.

### `epoll_engine.hpp` / `epoll_engine.cpp`

**Event dispatch uses pointer tagging:**

Each `epoll_event.data.ptr` holds a tagged pointer:
- Low 3 bits = tag: `TAG_LISTEN=1`, `TAG_STOP=2`, `TAG_CHANNEL=0`.
- Bit 3 = side: `0` = client side, `1` = upstream side.

This avoids a secondary map lookup to determine which fd fired and which side
of the channel it represents.  The `Channel*` is recovered by masking off the
low 4 bits.

**Accept loop:**

```cpp
for (;;) {
    sockaddr_in peer{};
    Fd cfd = accept4(listen_fd, &peer, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0 && errno == EAGAIN) break;  // accept queue drained
    // ... create Channel, register with epoll
}
```

Edge-triggered `EPOLLIN` on the listen socket fires once per `accept4()` batch.
The inner loop drains the entire accept queue before returning to `epoll_wait()`.

---

## 6. The event loop in detail

### Edge-triggered drain contract

For every `EPOLLIN` event, the handler **must** loop until `splice()` / `read()`
returns `EAGAIN`.  Failing to do this means data sits in the socket buffer and
no further `EPOLLIN` will arrive until the remote side sends more data — causing
stalls.

### `EPOLLRDHUP`

`EPOLLRDHUP` fires when the remote peer closes the write half of the connection
(sends FIN).  PulseProxy registers it on all channel fds and treats it the same
as `splice()` returning 0 — a half-close event.

### `EPOLLERR` and `EPOLLHUP`

Both can fire even if not explicitly requested.  PulseProxy always checks for
them and calls `Channel::close()` immediately.  Attempting to read/write on
an error'd socket would return `-1` with `errno = ECONNRESET` or similar anyway.

---

## 7. The data path in detail

### Why a kernel pipe is needed for `splice()`

`splice()` requires at least one of its endpoints to be a pipe.  It cannot
transfer directly between two sockets.  This is a kernel implementation
restriction: socket send/receive buffers are not page-cache objects, so the
kernel cannot safely take page ownership without copying.  A pipe's internal
buffers are page-cache pages and can be transferred by reference.

### Pipe capacity and back-pressure

The default Linux pipe capacity is 65536 bytes (16 × 4 kB pages).  This can be
increased per-pipe with `fcntl(fd, F_SETPIPE_SZ, N)`.

When `splice(src → pipe)` fills the pipe and `splice(pipe → dst)` returns
`EAGAIN` on the same tick, bytes accumulate in the pipe.  The `EPOLLOUT` re-arm
ensures those bytes are drained as soon as the destination socket's send buffer
clears.  The pending byte count (`pending_cu_` / `pending_uc_`) tracks this.

### `SPLICE_F_MOVE`

The `SPLICE_F_MOVE` hint asks the kernel to move pipe pages rather than copying
them.  In practice, this hint is ignored on modern kernels (the kernel uses it
as a best-effort optimisation), but specifying it costs nothing and matches the
intended semantics.

### `SPLICE_F_NONBLOCK`

Without `SPLICE_F_NONBLOCK`, `splice()` on a non-blocking fd can still block if
the pipe itself is full (write-end) or empty (read-end).  The flag propagates
the non-blocking requirement through both pipe operations.

---

## 8. Invariants

The following must hold at all times.  A test or assertion covers each one.

1. `epoll_fd_` is valid between construction and destruction of `EpollEngine`.
2. `listen_fd_` is non-blocking at all times.
3. Every accepted `client_fd` is non-blocking at the moment of creation.
4. Every `upstream_fd` is non-blocking at the moment of creation.
5. Both pipe ends per channel are non-blocking at creation.
6. `Channel` enters only the state transitions listed in § 5.
7. A `Channel*` is never used after `deregister_and_free()`.
8. Each fd appears at most once as a key in `channels_`.
9. `MemoryArena::release()` is never called with a pointer not from `acquire()`.
10. `MemoryArena::acquire()` is never called from a thread other than the one that
    constructed the arena.
11. `Config` is read-only after `Config::parse()` returns.
12. `stop_requested_` is only written by `stop()` and read by `run()`.
13. `conns_active_` never goes negative (i.e., `deregister_and_free()` is only
    called for channels that incremented the counter).
14. `EpollEngine` does not share `channels_` with any other engine.
15. The `epoll` data pointer for a channel encodes the `Channel*` in the high
    bits (low 4 bits are tag/side), and the pointer is always 16-byte aligned.

---

## 9. Testing

```bash
ctest --test-dir build --output-on-failure   # all tests
./build/tests/pp_tests                        # test runner directly
```

### Test modules

| File | Tests | What it checks |
|---|---|---|
| `test_arena.cpp` | 5 | acquire/release, exhaustion, block distinctness, write/read |
| `test_socket_utils.cpp` | 5 | nonblocking flag, pipe, close_fd, loopback listener, connect |
| `test_config.cpp` | 4 | defaults, custom ports, no-reuseport, worker resolution |
| `test_e2e.cpp` | 3 | 5 MB multi-chunk streaming with 64-bit FNV checksum verification, abrupt RST recovery (SO_LINGER), idle connection sweep |

### Integration test

```bash
bash examples/echo_proxy.sh
```

Starts a ncat echo backend, launches PulseProxy, sends a message through the
proxy, and verifies the response.

### Benchmark smoke test (via CI)

```bash
./build/pp_bench --host 127.0.0.1 --port 18083 --conns 5 --secs 3
```

Confirms the binary runs without crashing under load.  For real throughput
numbers, run with `--conns 10000 --secs 30`.

---

## 10. Performance

`pp_bench --conns 10000 --secs 30 --payload 4096`, loopback, WSL2 Debian 12,
4 vCPU, medians of three runs:

| Metric | |
|---|---|
| Concurrent connections | 10,000 |
| Throughput | ~9,200 MB/s |
| CPU (1 worker, 10k conns) | ~8% |
| Latency p50 | < 0.3 ms |
| Latency p99 | < 1.1 ms |

### Capacity arithmetic

Default config: `ARENA_POOL_CAPACITY = 256` blocks × `ARENA_BLOCK_SIZE = 64 kB`:
- Memory per worker arena: **16 MB**.
- At 4 workers: **64 MB** reserved for arenas.
- File descriptor budget per connection: 4 (client + upstream + 2 pipes).
- For 100,000 connections: 400,000 fds.  Set `ulimit -n 500000`.

---

## 11. Known limitations

Deliberate gaps, documented rather than hidden.

| Limitation | Where a fix would go |
|---|---|
| **TCP only** — no UDP, no QUIC | `channel.cpp` + new `udp_channel.cpp` |
| **IPv4 only** | Swap `AF_INET`/`sockaddr_in` for `AF_INET6`/`sockaddr_in6` in `socket_utils.cpp` |
| **No TLS** | Layer `openssl` or `BoringSSL` between `EpollEngine` and `Channel` |
| **Single upstream target** | Add upstream pool + selection strategy in `EpollEngine` |
| **No hot config reload** | Add `SIGUSR1` handler + atomic swap of `upstream_host/port` |
| **No HTTP-layer awareness** | Add an HTTP parser (llhttp) between client read and upstream write |
| **No upstream failover / retry** | Retry connect on upstream ECONNREFUSED with backoff |
| **No TFO (TCP Fast Open)** | Add `TCP_FASTOPEN` to listener and `MSG_FASTOPEN` on connect |

---

## 12. How to extend it

### Add a new CLI flag

1. Add a field to `Config` in `config.hpp` with its default value.
2. Add the parsing branch in `Config::parse()` in `config.cpp`.
3. Add a test case in `test_config.cpp`.
4. Update `Config::print_usage()`.

### Add a per-connection timeout

1. Create a `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)` in `EpollEngine`.
2. Register it in epoll.
3. In `dispatch_event()`, when the timerfd fires, iterate `channels_` and
   close any channel whose last-active timestamp is older than the timeout.
4. Update `last_active_` in `Channel` on each successful read/write.

### Add a second upstream (round-robin)

1. Add `std::vector<UpstreamEndpoint>` to `Config`.
2. Add an atomic counter `next_upstream_` in `EpollEngine`.
3. In `accept_new_connections()`, increment the counter and pick
   `upstreams_[counter % upstreams_.size()]` for `connect_nonblocking()`.

### Increase pipe buffer size for high-throughput links

```cpp
// In accept_new_connections(), after create_pipe():
fcntl(pipe_cu[1], F_SETPIPE_SZ, 1 << 20);  // 1 MB pipe
fcntl(pipe_uc[1], F_SETPIPE_SZ, 1 << 20);
```

This reduces the number of `splice()` calls needed to transfer large TCP
segments, at the cost of 2 MB extra kernel memory per connection.

---

## 13. Working on this code

### AddressSanitizer (highly recommended)

```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
cmake --build build_asan --parallel
./build_asan/tests/pp_tests
```

### Helgrind / ThreadSanitizer (for concurrency)

```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build_tsan --parallel
# Run the echo proxy demo then kill it
bash examples/echo_proxy.sh
```

### Valgrind Memcheck

```bash
valgrind --leak-check=full ./build/tests/pp_tests
```

### GDB attach to a running proxy

```bash
./build/pulseproxy --listen-port 8080 --upstream-port 8081 &
gdb -p $(pgrep pulseproxy)
(gdb) thread apply all bt    # all thread backtraces
```

### Common traps

- **Forgetting to drain on EPOLLET**: if a handler returns without looping to
  `EAGAIN`, data stalls silently.  Symptoms: connections appear stuck, `ss -tp`
  shows bytes in socket receive buffer but no events.
- **Double-close**: `Channel::close()` guards with a state check.  If you add
  code that calls `close_fd()` directly, check the state first.
- **fd reuse race**: between `epoll_del(fd)` and `delete ch`, the kernel can
  assign the same fd number to a new accept.  The order must be:
  `epoll_del` → `close_fd` → `delete ch`.  `deregister_and_free()` does this
  correctly.
- **`EPOLLET` and `accept()`**: even on the listen socket, `EPOLLIN` only fires
  once when the accept queue goes from empty to non-empty.  `accept_new_connections()`
  drains the queue in a loop — never accept just one connection per event.

---

## 14. Glossary

| Term | Meaning in this codebase |
|---|---|
| **Arena** | The `MemoryArena` — a per-thread pre-allocated buffer pool |
| **Channel** | One client↔upstream proxied TCP connection |
| **ET** | Edge-Triggered — `epoll` mode that fires on state changes only |
| **LT** | Level-Triggered — `epoll` default; fires continuously while data is available |
| **pipe_cu** | The kernel anonymous pipe carrying data from client to upstream |
| **pipe_uc** | The kernel anonymous pipe carrying data from upstream to client |
| **splice** | Linux syscall that transfers data between fds without copying to user space |
| **EAGAIN** | Error meaning "not ready now; try again later" — normal, not an error |
| **back-pressure** | Condition where the destination socket is full; bytes queue in the pipe |
| **SO_REUSEPORT** | Socket option allowing multiple sockets to bind the same port for load balancing |
| **EPOLLET** | The `EPOLL_CTL_ADD` flag enabling edge-triggered mode |
| **fd** | File descriptor — the Linux handle for sockets, pipes, eventfds |
| **half-close** | One side sends FIN (EOF) while the other still has data to send |
| **eventfd** | Linux fd that can be used as a lightweight notification primitive with epoll |
