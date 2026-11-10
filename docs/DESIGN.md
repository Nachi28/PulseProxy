# Design Notes

Decisions worth defending, and alternatives that were rejected.  Kept as a
running log so the reasoning survives past the commit that made the change.

---

## Edge-triggered `epoll`, not level-triggered

Level-triggered (the default) fires every `epoll_wait()` call as long as a
socket has pending data.  Under sustained load on thousands of connections, this
means the event loop wakes up repeatedly for sockets it already processed,
burning CPU on redundant system calls.

Edge-triggered fires once per state change.  The requirement: drain each socket
completely in a tight loop until `EAGAIN`.  PulseProxy does this for every
readable event.  The result is a quieter event loop — `epoll_wait()` sleeps
until there is genuinely new work.

The implementation cost is higher: you must handle the `EAGAIN` case cleanly
and arm `EPOLLOUT` for back-pressure correctly.  That is worth it — the
alternative is O(N) wakeups per tick on a busy server.

---

## `splice()` over `read()`/`write()`

The rejected approach is the standard proxy loop:

```cpp
char buf[65536];
ssize_t n = read(src_fd, buf, sizeof(buf));  // kernel → user space
write(dst_fd, buf, n);                        // user space → kernel
```

This is correct but wasteful: every byte crosses the kernel/user boundary
twice, polluting caches with payload data the proxy has no business inspecting.

`splice()` moves page references in kernel space.  The CPU never touches the
bytes.  The only requirement is an anonymous kernel pipe sitting between the two
sockets — the data path becomes:

```
socket buffer → pipe (page ref) → socket buffer   (zero memcpy)
```

The limitation: `splice()` on sockets requires a pipe intermediary because the
kernel cannot transfer directly between two socket buffers (as of Linux 5.15).
Two file descriptors are created per channel.  On 100,000 connections that is
400,000 open file descriptors for pipes alone — the ulimit `nofile` must be set
accordingly (see HANDBOOK.md § Invariants).

---

## `SO_REUSEPORT` for load balancing, not a shared accept queue

The classic multi-process/thread server uses a single listener and all workers
race to call `accept()`.  The thundering-herd problem: a new connection wakes
every blocked `accept()` and all but one go back to sleep, burning CPU.

Linux 3.9 added `SO_REUSEPORT`: multiple sockets on the same port, with the
kernel performing a 4-tuple hash to deliver each SYN to exactly one listener.
No shared state.  No mutex.  Each worker's `accept4()` only ever sees its own
SYNs.

The tradeoff: if one worker is busy and another is idle, the kernel cannot
rebalance in-flight connections.  For a proxy where connection setup is fast
and all workers are homogeneous, this is irrelevant in practice.

---

## Per-thread `MemoryArena`, not a shared pool

A global lock-free allocator (like `jemalloc` per-thread caches) would work,
but it would require cross-thread synchronisation on every acquire/release if
channels migrate between threads — which they cannot in this design.

PulseProxy pins each channel to one worker for its entire lifetime.  With that
guarantee, a per-thread arena needs zero synchronisation.  The free-list is a
plain LIFO stack with no atomics.  The arena is allocated once at worker start
and never handed a pointer from another thread.

---

## `eventfd` for shutdown signalling, not a signal handler writing to a pipe

The classic pattern for waking `epoll_wait()` from a signal is to write a byte
to a pipe and add the read end to the epoll set.  This works, but it requires
two file descriptors and careful handling of `SA_RESTART`.

`eventfd(EFD_NONBLOCK)` is a single file descriptor that can be waited on by
`epoll_wait()`.  Writing 1 to it from `stop()` wakes every worker whose epoll
instance is watching it.  It is simpler, allocates half as many file
descriptors, and is the approach used by modern event libraries (libuv, Boost.Asio).

---

## Direct flat-array FD table (`fd_table_`), not an associative hash map

An `std::unordered_map<Fd, Channel*>` is the textbook solution. It works, but
every socket event incurs a hash computation, bucket lookup, and node pointer
dereference that can stall L1 cache lines.

Linux file descriptors are small, contiguous integers allocated from 0 up.
PulseProxy uses an `std::vector<Channel*> fd_table_` directly indexed by `fd`.
Looking up a connection is a single pointer read: `fd_table_[fd]`. It costs
zero hash computations, creates zero node allocations on the heap, and has
optimal cache locality. The table grows dynamically only when new descriptors
exceed its current bounds.

A separate dense vector `active_channels_` maintains pointers for $O(1)$ swap-and-pop
removal and fast iteration during periodic idle connection sweeps.

---

## Channel owns pipes; EpollEngine owns Channels

Ownership is strict and linear:

```
EpollEngine → Channel → { client_fd, upstream_fd, pipe_cu[2], pipe_uc[2] }
```

No shared ownership, no reference counting.  When EpollEngine decides a channel
is closed, it calls `delete ch` and all file descriptors are closed in
`Channel::~Channel()`.  This makes lifetime bugs easy to reason about with ASAN.

---

## Back-pressure: pending pipe counters + `EPOLLOUT` re-arm

When `splice(src → pipe)` succeeds but `splice(pipe → dst)` returns `EAGAIN`
(the destination socket's send buffer is full), bytes sit in the kernel pipe.
They must not be lost.

The design:
1. The second `splice()` returning `EAGAIN` triggers `EPOLLOUT` registration on
   `dst_fd`.
2. When `dst_fd` becomes writable, the `on_*_writable()` handler drains the
   pipe by re-running `on_*_readable()`.
3. If the pipe is now empty, `EPOLLOUT` is removed from the interest list.

This avoids busy-polling the destination socket and handles arbitrarily slow
consumers without dropping data.
