# 🗺️ Project Roadmap & Task Tracker

> **Build with learning, learn with building.**
> Each task has four parts:
>
> - **📖 Learn:** concepts to study *before* writing any code
> - **🧪 Experiment:** a small throwaway program in `learn/` to try the concept on its own
> - **🔨 Build:** what to add to the real project in `src/`
> - **✅ Done when:** a concrete check that tells you the task is finished
>
> Each phase ends with a **🎓 Checkpoint**: questions you should be able to answer out loud without notes. If you can't, go back and re-study before moving on.
>
> Mark progress with `[x]`. Write notes in `notes/<phase>-<topic>.md`, since explaining something in writing is the best test of understanding it.

---

## 📊 Progress Overview

| Phase | Topic | Status |
|---|---|---|
| 0 | Setup, C refresher & tooling | 🟨 |
| 1 | Networking & socket programming | ⬜ |
| 2 | HTTP/1.1 & parsing | ⬜ |
| 3 | Concurrency fundamentals (threads, races, locks, semaphores) | ⬜ |
| 4 | Async I/O & the event loop | ⬜ |
| 5 | Multi-core: multi-reactor & lock-free basics | ⬜ |
| 6 | Reverse proxy | ⬜ |
| 7 | Configuration system | ⬜ |
| 8 | Load balancing & health checks | ⬜ |
| 9 | LRU caching | ⬜ |
| 10 | SSL/TLS termination | ⬜ |
| 11 | Performance engineering | ⬜ |
| 12 | Production hardening | ⬜ |

Legend: ⬜ not started · 🟨 in progress · ✅ done

---

## Phase 0: Setup, C Refresher & Tooling

*Goal: a clean project skeleton and the debugging tools you'll rely on for the rest of the project.*

### [ ] 0.1 Archive the old code and start fresh
- **📖 Learn:** Git branching, tags, `git mv`, `.gitignore` for C projects (don't commit build artifacts or binaries).
- **🔨 Build:** Tag the current state (`git tag v0-legacy`), then remove the old `.c`/`.h` files and the committed `proxy` binary. Create the folder layout from the README (`src/`, `include/`, `tests/`, `learn/`, `notes/`, `bench/`).
- **✅ Done when:** `git checkout v0-legacy` still shows the old code, and `main` has only the empty skeleton and docs.

### [ ] 0.2 C refresher
- **📖 Learn:**
  - Pointers, pointer arithmetic, pointers to pointers, function pointers (used later for callbacks in the event loop)
  - Stack vs heap, `malloc`/`calloc`/`realloc`/`free`, memory leaks, dangling pointers, double free
  - `struct`, `union`, `enum`, `typedef`, struct padding and alignment
  - Strings in C: why `strcpy`/`sprintf` are dangerous, and how to use `snprintf`/`memcpy` safely with explicit lengths
  - Header files, `static` functions, include guards, the compilation pipeline (preprocess → compile → assemble → link)
  - `errno`, return-code error handling, `goto cleanup` pattern
- **🧪 Experiment:** Write a dynamic array (`vec`) and a growable byte buffer (`buf_append`, `buf_consume`). You will reuse the buffer in Phase 4.
- **✅ Done when:** Both work under AddressSanitizer with no leaks or errors.

### [ ] 0.3 Build system
- **📖 Learn:** Makefiles (targets, dependencies, variables, pattern rules), then CMake basics (`add_executable`, `target_include_directories`, build types), and compiler flags `-Wall -Wextra -Werror -O2 -g`.
- **🔨 Build:** `CMakeLists.txt` with Debug (sanitizers on) and Release (`-O2`/`-O3`) builds, plus a `src/main.c` that prints the version.
- **✅ Done when:** `cmake --build build` produces a binary in both modes.

### [ ] 0.4 Debugging & sanitizer toolkit
- **📖 Learn:** `lldb` (macOS) / `gdb` (Linux): breakpoints, backtraces, stepping, inspecting memory. AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer (you'll need it in Phase 3). Valgrind (Linux only). Core dumps.
- **🧪 Experiment:** Write programs with deliberate bugs (buffer overflow, use-after-free, uninitialized read) and catch each one with the right tool.
- **✅ Done when:** You can find a segfault's cause with a debugger in under 5 minutes.

### [ ] 0.5 Linux environment (recommended)
- **📖 Learn:** Why Linux matters here: `epoll`, `perf`, Valgrind and `splice` are Linux-only. Docker or a VM (OrbStack, UTM, Multipass) on macOS.
- **🔨 Build:** A Docker or VM development environment with gcc, cmake, openssl and wrk.
- **✅ Done when:** The skeleton builds on both macOS and Linux.

### 🎓 Checkpoint 0
- What is the difference between the stack and the heap? When does each get freed?
- What does `-fsanitize=address` actually do at runtime?
- Why is `char buf[10]; strcpy(buf, user_input);` a security vulnerability?

---

## Phase 1: Networking & Socket Programming

*Goal: understand what happens between `socket()` and `close()`, and build a working (slow) blocking server.*

### [ ] 1.1 Networking fundamentals
- **📖 Learn:**
  - OSI and TCP/IP models, and where HTTP, TLS, TCP and IP fit
  - IP addresses, ports, IPv4 vs IPv6
  - TCP vs UDP; the TCP 3-way handshake, 4-way teardown, `TIME_WAIT`
  - TCP is a **byte stream**, not a stream of messages, so one `recv` ≠ one message. This is the most important networking fact for this project.
  - Flow control, Nagle's algorithm, the kernel send/receive buffers
- **🧪 Experiment:** Use `tcpdump`/Wireshark to capture a `curl http://example.com` and identify the handshake, data and FIN packets.
- **✅ Done when:** You can draw the TCP state diagram from memory (at least `LISTEN`, `SYN_RCVD`, `ESTABLISHED`, `FIN_WAIT`, `TIME_WAIT`, `CLOSE_WAIT`).

### [ ] 1.2 Sockets API
- **📖 Learn:** `socket`, `bind`, `listen`, `accept`, `connect`, `send`/`recv`, `read`/`write`, `close`, `shutdown`. File descriptors ("everything is a file"). `struct sockaddr_in`/`sockaddr_in6`/`sockaddr_storage`. Byte order (`htons`, `ntohl`). `getaddrinfo` (DNS resolution). The `listen` backlog. `SO_REUSEADDR`.
- **📚 Resources:** Beej's Guide (chapters 1–6), UNP chapters 1–5.
- **🧪 Experiment:** A TCP **echo server** and an echo **client**, both blocking.
- **✅ Done when:** `nc localhost 8080` echoes back everything you type, and restarting the server immediately doesn't fail with "Address already in use".

### [ ] 1.3 Robust I/O
- **📖 Learn:** Partial reads and **partial writes** (`send` may write fewer bytes than asked), `EINTR`, `SIGPIPE` and `MSG_NOSIGNAL`/`SO_NOSIGPIPE`, detecting peer close (`recv` returns 0), `ECONNRESET`.
- **🔨 Build:** `src/net/`: helper functions `send_all()` and `listen_on(port)` with proper error handling.
- **✅ Done when:** The server survives a client disconnecting in the middle of a response (test with `nc` + Ctrl-C).

### [ ] 1.4 First blocking HTTP server
- **🔨 Build:** Accept one connection at a time, read until `\r\n\r\n`, and reply with a hard-coded `HTTP/1.1 200 OK` response.
- **✅ Done when:** `curl -v localhost:8080` and a browser both show your response.
- **📏 Measure:** `wrk -t2 -c50 -d10s http://localhost:8080/`. Write the result in `bench/RESULTS.md` as the **baseline**. Note how throughput collapses as concurrency rises.

### 🎓 Checkpoint 1
- Why might `recv()` return only half of an HTTP request?
- What is `TIME_WAIT` for, and why does `SO_REUSEADDR` help during development?
- What happens to a second client while the server is busy with the first one, and why?

---

## Phase 2: HTTP/1.1 & Parsing

*Goal: a correct, fast, safe HTTP/1.1 request parser built as a state machine.*

### [ ] 2.1 HTTP/1.1 protocol
- **📖 Learn:** Request line, headers, body; methods; status codes; `Content-Length` vs `Transfer-Encoding: chunked`; persistent connections (keep-alive) and `Connection: close`; pipelining; `Host` header; header case-insensitivity.
- **📚 Resources:** RFC 9110 (semantics), RFC 9112 (message syntax). Skim them, then come back when you need details.
- **🧪 Experiment:** `curl -v`, and `nc example.com 80` with a hand-typed request.

### [ ] 2.2 Finite state machines
- **📖 Learn:** What an FSM is; why parsers for streamed input must be **incremental** (they must be able to stop mid-header and resume when more bytes arrive); zero-copy parsing (store pointer + length into the buffer instead of copying strings).
- **📚 Resources:** Read the design of `picohttpparser` and `llhttp`, but write your own.

### [ ] 2.3 Build the HTTP parser
- **🔨 Build:** `src/http/parser.c`, an incremental request parser:
  - Request line → method, path, version
  - Headers into a fixed array of `{name, name_len, value, value_len}` slices
  - Body framing: `Content-Length` and chunked decoding
  - Limits: max header size, max header count, max URI length (reject with `400`/`413`/`414`/`431`)
- **✅ Done when:** Unit tests pass for: a whole request in one buffer, a request split byte-by-byte, pipelined requests, malformed input, and oversized headers.

### [ ] 2.4 Response builder & keep-alive
- **🔨 Build:** `src/http/response.c`: build status line + headers + body into an output buffer. Support keep-alive: after one response, loop back to parse the next request on the same connection.
- **✅ Done when:** `wrk` (which uses keep-alive) works, and `curl` with multiple URLs reuses one connection (look for "Re-using existing connection" in `curl -v`).

### [ ] 2.5 Fuzz the parser
- **📖 Learn:** What fuzzing is; libFuzzer / AFL++; coverage-guided fuzzing.
- **🔨 Build:** `tests/fuzz_parser.c`.
- **✅ Done when:** 10 minutes of fuzzing with ASan finds no crashes.

### 🎓 Checkpoint 2
- How does the server know where one request's body ends and the next request begins?
- What is HTTP request smuggling, and how do `Content-Length` + `Transfer-Encoding` conflicts cause it?
- Why does zero-copy parsing make the server faster?

---

## Phase 3: Concurrency Fundamentals ⭐

*Goal: learn threads and synchronization properly, then see for yourself why thread-per-connection doesn't scale. **This is where race conditions, mutexes and semaphores are covered.***

### [ ] 3.1 Processes vs threads
- **📖 Learn:** `fork()`, `exec`, `wait`, zombie processes; what a thread shares (heap, globals, file descriptors) vs owns (stack, registers); context switching cost; user vs kernel threads.
- **🧪 Experiment:** Make the blocking server fork a child per connection. Then use `pthread_create` with one thread per connection.
- **📏 Measure:** Benchmark both against the Phase 1 baseline.

### [ ] 3.2 Race conditions
- **📖 Learn:** Shared mutable state; **critical sections**; why `counter++` is not atomic (read-modify-write); data races vs race conditions; non-determinism; Heisenbugs.
- **🧪 Experiment:** 8 threads each increment a shared counter 1,000,000 times. Observe a wrong total. Run it under **ThreadSanitizer** and read the report.
- **✅ Done when:** You can explain at the instruction level exactly why the count is wrong.

### [ ] 3.3 Mutexes
- **📖 Learn:** `pthread_mutex_t`, lock/unlock, mutual exclusion, lock granularity (coarse vs fine), lock contention, `pthread_mutex_trylock`, recursive mutexes (and why to avoid them).
- **🧪 Experiment:** Fix the counter with a mutex. Measure how much slower it is than the broken version, and why.

### [ ] 3.4 Deadlocks
- **📖 Learn:** The 4 Coffman conditions; lock ordering; livelock; starvation; priority inversion.
- **🧪 Experiment:** Write a program that deadlocks with two mutexes, attach a debugger, and inspect each thread's backtrace. Fix it with lock ordering.
- **📚 Resources:** The dining philosophers problem.

### [ ] 3.5 Condition variables
- **📖 Learn:** `pthread_cond_wait`/`signal`/`broadcast`; why `wait` takes a mutex; **spurious wakeups**, so always wait in a `while` loop; lost-wakeup bug.
- **🧪 Experiment:** One thread waits until another sets a "ready" flag.

### [ ] 3.6 Semaphores
- **📖 Learn:** Counting vs binary semaphores; `sem_init`/`sem_wait`/`sem_post` (note: unnamed POSIX semaphores are deprecated on macOS, so use `dispatch_semaphore` or build your own from mutex + condvar); semaphore vs mutex (ownership); using a semaphore to **limit concurrency** (e.g. max N active clients).
- **📚 Resources:** *The Little Book of Semaphores*: work through the rendezvous, barrier, and producer–consumer puzzles.
- **🧪 Experiment:** Implement your own counting semaphore using a mutex + condition variable.
- **✅ Done when:** Your semaphore passes a stress test under ThreadSanitizer.

### [ ] 3.7 Producer–consumer & the thread pool
- **📖 Learn:** Bounded buffer problem; blocking queue; thread pool pattern; back-pressure (what happens when the queue is full?).
- **🔨 Build:** `src/core/threadpool.c`: a fixed pool of worker threads that pull accepted client fds from a bounded queue (mutex + 2 condvars, or semaphores). Use it in the server: the main thread `accept()`s and pushes, workers handle.
- **✅ Done when:** It runs clean under TSan with `wrk -c200`, and it shuts down cleanly (all workers join, nothing leaks).
- **📏 Measure:** Compare with thread-per-connection. Then try `-c10000`. What happens to memory and latency?

### [ ] 3.8 Reader–writer locks & atomics preview
- **📖 Learn:** `pthread_rwlock_t` and when reads vastly outnumber writes (this matters for the cache in Phase 9). A first look at `<stdatomic.h>` (`atomic_fetch_add`).
- **🧪 Experiment:** Redo the counter with `atomic_fetch_add` and compare its speed to the mutex version.

### [ ] 3.9 The C10K problem
- **📖 Learn:** Read Dan Kegel's *C10K Problem*. Why 10,000 threads is a bad idea: stack memory, context switches, scheduler overhead, cache thrashing.
- **✅ Done when:** You can explain in your notes why the thread pool server can't handle 10k mostly idle keep-alive connections, which sets up Phase 4.

### 🎓 Checkpoint 3
- What's the difference between a mutex and a binary semaphore?
- Why must `pthread_cond_wait` be called inside a `while` loop?
- Give a real example of deadlock in a proxy server, and how you'd prevent it.
- What happens in your thread pool when all workers are blocked on slow clients?

---

## Phase 4: Async I/O & the Event Loop ⭐

*Goal: the core of the project, a single-threaded, non-blocking, event-driven server.*

### [ ] 4.1 I/O models
- **📖 Learn:** The 5 I/O models (UNP chapter 6): blocking, non-blocking, I/O multiplexing, signal-driven, async (`io_uring`). `O_NONBLOCK` with `fcntl`; `EAGAIN`/`EWOULDBLOCK`.
- **🧪 Experiment:** Set a socket non-blocking and watch `recv` return `EAGAIN`.

### [ ] 4.2 `select` and `poll`
- **📖 Learn:** How they work, the `FD_SETSIZE` limit, and why both are O(n) for each call.
- **🧪 Experiment:** Echo server with `poll()` handling many clients in one thread.

### [ ] 4.3 `epoll` (Linux) and `kqueue` (macOS)
- **📖 Learn:** `epoll_create1`, `epoll_ctl`, `epoll_wait`; `kqueue`, `kevent`, `EVFILT_READ`/`EVFILT_WRITE`. **Level-triggered vs edge-triggered** (with edge-triggered you must read until `EAGAIN`). `EPOLLONESHOT`. Why they are O(1) per ready event.
- **🧪 Experiment:** Echo server with each one. Deliberately cause the edge-triggered "missed data" bug.

### [ ] 4.4 Event loop abstraction
- **📖 Learn:** The **Reactor pattern**; callbacks and function pointers; how libuv, libevent and nginx structure their loops.
- **🔨 Build:** `src/core/event_loop.c` with a backend-neutral API: `loop_create`, `loop_add(fd, events, callback, userdata)`, `loop_mod`, `loop_del`, `loop_run`. Write two backends: `ev_epoll.c` and `ev_kqueue.c`, chosen at compile time.
- **✅ Done when:** The same echo server runs on macOS and Linux.

### [ ] 4.5 Connection state machine & buffers
- **📖 Learn:** Why every connection needs explicit state (you can no longer keep state in local variables on a blocking thread's stack); read/write buffers; only enabling write interest when there's pending output.
- **🔨 Build:** `src/net/connection.c`: `struct conn { fd, state, rbuf, wbuf, parser, ... }`. States: `READING → PROCESSING → WRITING → (keep-alive) READING | CLOSING`. Plug in the Phase 2 parser.
- **✅ Done when:** It serves HTTP with keep-alive and handles slow clients that send one byte at a time.

### [ ] 4.6 Timers & timeouts
- **📖 Learn:** Why timeouts are mandatory (idle connections, slowloris attacks); min-heap vs **timer wheel**; monotonic clock (`clock_gettime(CLOCK_MONOTONIC)`); using the timer to compute the `epoll_wait` timeout.
- **🔨 Build:** `src/core/timer.c`, then add header-read timeout, keep-alive idle timeout and write timeout.
- **✅ Done when:** A connection that sends nothing is closed after N seconds.

### [ ] 4.7 Accepting efficiently
- **📖 Learn:** Non-blocking listening socket; accepting in a loop until `EAGAIN`; `accept4` with `SOCK_NONBLOCK`; handling `EMFILE` (out of fds); `ulimit -n`.
- **✅ Done when:** It handles 10,000 concurrent idle connections (test with a script or `wrk -c10000`) with low memory.
- **📏 Measure:** Compare with the thread pool. Record the req/s and memory use per connection.

### 🎓 Checkpoint 4
- Explain the edge-triggered vs level-triggered difference with a concrete bug example.
- Why is an event loop single-threaded, and what happens if one callback blocks?
- How does one thread handle 10,000 connections?

---

## Phase 5: Multi-Core: Multi-Reactor & Lock-Free Basics

*Goal: use all CPU cores without losing the benefits of the event loop.*

### [ ] 5.1 Scaling models
- **📖 Learn:** nginx model (multi-process), multi-reactor (one loop per thread), the thundering herd problem, `SO_REUSEPORT` (kernel load-balances `accept`), passing fds between threads via a pipe/eventfd.
- **📚 Resources:** *Inside NGINX: How We Designed for Performance & Scale*.

### [ ] 5.2 Build the multi-reactor
- **🔨 Build:** N worker threads, each with its own event loop and its own `SO_REUSEPORT` listening socket. Design rule: **share nothing** by default; each worker owns its connections.
- **✅ Done when:** Throughput scales close to linearly with cores (measure 1, 2, 4, N workers).

### [ ] 5.3 Atomics & memory ordering
- **📖 Learn:** C11 `<stdatomic.h>`; `memory_order_relaxed` / `acquire` / `release` / `seq_cst`; CPU reordering; compare-and-swap (CAS); the ABA problem; spinlocks vs mutexes.
- **🧪 Experiment:** Build a spinlock with `atomic_flag`, and a lock-free single-producer single-consumer ring buffer.

### [ ] 5.4 Cache-friendly concurrency
- **📖 Learn:** CPU caches and cache lines; **false sharing**; per-thread counters merged when read; CPU affinity (`pthread_setaffinity_np` on Linux).
- **🔨 Build:** Per-worker stats (requests, bytes, active connections), padded to 64 bytes and summed only when reported.
- **🧪 Experiment:** Show false sharing with two threads updating adjacent counters, then fix it with padding.

### [ ] 5.5 Signals & graceful shutdown
- **📖 Learn:** Signal handlers and async-signal-safety; the self-pipe trick / `signalfd` / `EVFILT_SIGNAL`; `pthread_sigmask`.
- **🔨 Build:** SIGINT/SIGTERM stop accepting, let in-flight requests finish, then exit cleanly.
- **✅ Done when:** Ctrl-C during a `wrk` run produces no broken responses and no leaks.

### 🎓 Checkpoint 5
- What is the thundering herd problem, and how does `SO_REUSEPORT` avoid it?
- When is `memory_order_relaxed` safe?
- What is false sharing, and how did you measure it?

---

## Phase 6: Reverse Proxy 🔁

*Goal: forward requests to upstream servers asynchronously and stream responses back.*

### [ ] 6.1 Proxy concepts
- **📖 Learn:** Forward vs reverse proxy; what nginx/HAProxy do; hop-by-hop headers (`Connection`, `Keep-Alive`, `Transfer-Encoding`, `Upgrade`, ...); `X-Forwarded-For`, `X-Forwarded-Proto`, `Forwarded`, `Via`; preserving vs rewriting `Host`.
- **🧪 Experiment:** Put nginx in front of a Python `http.server` and inspect the forwarded headers.

### [ ] 6.2 Test backends
- **🔨 Build:** `tests/backends/`: tiny backends (e.g. Python or your own Phase 4 server) on ports 9001–9003 that reply with their port number, a configurable delay, and configurable response sizes.

### [ ] 6.3 Non-blocking upstream connect
- **📖 Learn:** Non-blocking `connect()` returns `EINPROGRESS`; wait for writability; check `SO_ERROR`; connect timeouts. DNS is blocking (`getaddrinfo`), so resolve at startup or use a thread pool (reuse Phase 3!).
- **🔨 Build:** `src/proxy/upstream.c`: connect to the upstream as another fd in the same event loop.

### [ ] 6.4 Forwarding & streaming
- **📖 Learn:** Two linked connections (client ↔ upstream) in one state machine; **back-pressure** (stop reading from upstream when the client's write buffer is full); streaming large bodies instead of buffering them all; chunked encoding pass-through.
- **🔨 Build:** `src/proxy/forward.c`: rewrite request headers → send upstream → parse the response head → stream the body to the client.
- **✅ Done when:** It proxies a 1 GB download with flat memory use, and handles slow clients without growing buffers without limit.

### [ ] 6.5 Upstream connection pooling
- **📖 Learn:** Why reusing connections matters (TCP + TLS handshake cost); keep-alive to upstreams; detecting stale pooled connections; per-worker pools (no locks needed).
- **🔨 Build:** `src/proxy/pool.c`.
- **📏 Measure:** req/s with and without pooling.

### [ ] 6.6 Error handling
- **🔨 Build:** `502 Bad Gateway` (connect failed / bad response), `504 Gateway Timeout` (upstream too slow), client disconnects mid-proxy (clean up both sides), upstream closes mid-response.
- **✅ Done when:** Killing a backend during a load test never crashes or leaks in the proxy.

### 🎓 Checkpoint 6
- Why must hop-by-hop headers not be forwarded?
- What is back-pressure, and what breaks without it?
- How do you avoid leaking the upstream fd when the client disconnects?

---

## Phase 7: Configuration System

*Goal: drive behavior from a config file instead of hard-coded values.*

### [ ] 7.1 Lexing & parsing
- **📖 Learn:** Tokenizer/lexer; recursive descent parser; error messages with line numbers.
- **🔨 Build:** `src/config/`: parse the nginx-like format shown in the README (listen, upstream blocks, routes, cache, ssl).
- **✅ Done when:** Bad configs produce helpful errors (`proxy.conf:12: unknown directive 'srever'`), and `proxy -t` validates a config without starting the server.

### [ ] 7.2 Routing
- **📖 Learn:** Prefix matching, longest-prefix match, tries / radix trees; host-based routing.
- **🔨 Build:** Map `(host, path)` → route → upstream group + cache policy.

### [ ] 7.3 Hot reload (stretch)
- **📖 Learn:** SIGHUP reload; immutable config snapshots + atomic pointer swap; RCU-style reclamation (when is it safe to free the old config?).
- **✅ Done when:** Reloading during a load test drops zero requests.

---

## Phase 8: Load Balancing & Health Checks ⚖️

*Goal: spread traffic across backends intelligently and route around failures.*

### [ ] 8.1 Load-balancing theory
- **📖 Learn:** L4 vs L7 load balancing; stateless vs stateful algorithms; session affinity (sticky sessions); trade-offs of each algorithm below.

### [ ] 8.2 Round robin & weighted round robin
- **📖 Learn:** Plain RR; **smooth weighted round robin** (nginx's algorithm: why naive weighted RR sends bursts to one server). With a multi-reactor, should the counter be per-worker or a shared atomic?
- **🔨 Build:** `src/lb/` with a strategy interface (function pointers: `pick(group, request)`), plus `rr` and `wrr`.
- **✅ Done when:** Requests split according to weights (verify via backend logs).

### [ ] 8.3 Least connections
- **📖 Learn:** Tracking active connections per backend across threads (atomics); power-of-two-choices (P2C) and why it works so well.
- **🔨 Build:** `least_conn` and `p2c`.
- **📏 Measure:** Give one backend a 200 ms delay. Compare latency for RR vs least-conn.

### [ ] 8.4 Consistent hashing
- **📖 Learn:** Hash functions (FNV-1a, xxHash, MurmurHash); modulo hashing and why adding a server reshuffles everything; the consistent hash ring with virtual nodes; Maglev / Jump hash (bonus).
- **🔨 Build:** `ip_hash` / `hash $uri` strategy.
- **✅ Done when:** Removing 1 of 4 backends remaps only ~25% of keys.

### [ ] 8.5 Health checks
- **📖 Learn:** Passive (mark a backend down after N failures) vs active (periodic `GET /health`); thresholds (`rise`/`fall`); state shared across workers (atomics).
- **🔨 Build:** Active health checker using timers + the event loop; passive failure counting in the proxy path.
- **✅ Done when:** Killing a backend removes it within seconds, and restarting it brings it back automatically.

### [ ] 8.6 Retries & circuit breaking
- **📖 Learn:** Which requests are safe to retry (idempotent methods); retry budgets; circuit breaker states (closed / open / half-open).
- **🔨 Build:** Retry on connect failure to the next backend; per-backend circuit breaker.

### 🎓 Checkpoint 8
- Why does smooth WRR exist?
- Why is P2C almost as good as true least-connections but much cheaper?
- What happens to cache-hit ratios (Phase 9) if you use consistent hashing across multiple proxy instances?

---

## Phase 9: LRU Caching 🗄️

*Goal: serve repeated requests from memory with no upstream round trip.*

### [ ] 9.1 Cache data structures
- **📖 Learn:** Hash tables (open addressing vs chaining, load factor, resizing); doubly linked lists; why **hash map + doubly linked list** gives O(1) get/put/evict; other policies: LFU, ARC, CLOCK, W-TinyLFU (read about them for comparison).
- **🧪 Experiment:** A single-threaded LRU in `learn/` with unit tests (LeetCode 146 is the same problem).
- **✅ Done when:** All operations are O(1) and verified under ASan.

### [ ] 9.2 HTTP caching semantics
- **📖 Learn:** RFC 9111: `Cache-Control` (`max-age`, `s-maxage`, `no-store`, `no-cache`, `private`, `public`), `Expires`, `Age`; `ETag` / `If-None-Match` and `Last-Modified` / `If-Modified-Since` → `304`; the `Vary` header; which methods and status codes are cacheable; cache keys (method + host + path + query + Vary headers).
- **✅ Done when:** Your notes have a decision table: "given this response, cache it? for how long?"

### [ ] 9.3 Concurrent cache
- **📖 Learn:** One global lock = contention; **sharding** (N shards, each with its own lock and LRU); mutex vs rwlock for LRU (a "read" moves the node to the front, so it's really a write!); **reference counting** so an entry being streamed to a client isn't freed when it's evicted; memory accounting and byte-based limits.
- **🔨 Build:** `src/cache/lru.c`: sharded, thread-safe LRU with ref-counted entries, max memory size, and TTL expiry.
- **✅ Done when:** A TSan stress test (many threads doing get/put/evict) is clean.

### [ ] 9.4 Integrate with the proxy
- **🔨 Build:** Cache lookup before choosing an upstream; store responses while streaming them back (tee); add an `X-Cache: HIT/MISS` header; enforce a max object size.
- **📖 Learn:** The **cache stampede / thundering herd** problem: 1,000 simultaneous misses for the same key → 1,000 upstream requests. Request coalescing (one request goes upstream, the rest wait).
- **🔨 Build:** Request coalescing for in-flight misses.
- **✅ Done when:** 1,000 concurrent requests for a cold key cause exactly **1** upstream request.
- **📏 Measure:** req/s for cache hits vs proxied requests.

### 🎓 Checkpoint 9
- Why does an LRU `get` need a write lock?
- How does reference counting prevent a use-after-free during eviction?
- What does `Vary: Accept-Encoding` mean for your cache key?

---

## Phase 10: SSL/TLS Termination 🔒

*Goal: accept HTTPS from clients and talk plain HTTP to backends.*

### [ ] 10.1 Cryptography basics
- **📖 Learn:** Symmetric vs asymmetric encryption; hashing; MACs; Diffie-Hellman key exchange; digital signatures; certificates, CAs and chains of trust; forward secrecy.
- **📚 Resources:** *Serious Cryptography* (J.-P. Aumasson); *The Illustrated TLS 1.3 Connection* (tls13.xargs.org).

### [ ] 10.2 TLS protocol
- **📖 Learn:** TLS 1.2 vs 1.3 handshakes; ClientHello / ServerHello; **SNI**; **ALPN**; session resumption (tickets, PSK); 0-RTT and replay risks; what "termination" means (the proxy decrypts, backends get plain HTTP).
- **🧪 Experiment:** `openssl s_client -connect example.com:443 -servername example.com` and read the output. Capture a handshake in Wireshark.

### [ ] 10.3 Certificates for development
- **🔨 Build:** Generate a local CA + server certificate with `openssl` (or `mkcert`) into `certs/` (gitignored!).

### [ ] 10.4 OpenSSL in blocking mode
- **📖 Learn:** `SSL_CTX`, `SSL`, `SSL_accept`, `SSL_read`, `SSL_write`, error handling with `SSL_get_error` and `ERR_print_errors`.
- **🧪 Experiment:** A blocking TLS echo server in `learn/`.

### [ ] 10.5 Non-blocking TLS in the event loop
- **📖 Learn:** `SSL_ERROR_WANT_READ` / `SSL_ERROR_WANT_WRITE` (a TLS *read* may need a socket *write*, e.g. during renegotiation or key update); handshake as a connection state; `SSL_pending` (data already decrypted and buffered inside OpenSSL); memory BIOs vs socket BIOs.
- **🔨 Build:** `src/tls/`: a transport abstraction (`conn->io.read/write` function pointers) so HTTP code doesn't care whether it's TLS or plain TCP.
- **✅ Done when:** `curl -v --cacert certs/ca.crt https://localhost:8443/` works through the proxy to a backend, and `wrk` over HTTPS works.

### [ ] 10.6 Production TLS features
- **🔨 Build:** SNI with a certificate per hostname; TLS 1.2+ only with modern ciphers; session tickets for resumption; ALPN (`http/1.1` for now); HTTP → HTTPS redirect listener.
- **✅ Done when:** testssl.sh or `sslyze` against localhost shows no weak protocols or ciphers.
- **📏 Measure:** Handshakes/sec with full handshakes vs resumed sessions. Where does CPU time go?

### 🎓 Checkpoint 10
- Why can `SSL_read` return `WANT_WRITE`?
- What does SNI solve, and why is it sent unencrypted (and what is ECH)?
- Why are TLS handshakes expensive, and how does session resumption help?

---

## Phase 11: Performance Engineering 🏎️

*Goal: make it fast, and prove it with numbers.*

### [ ] 11.1 Benchmarking methodology
- **📖 Learn:** Throughput vs latency; **percentiles (p50/p99/p999)** and why averages lie; coordinated omission (`wrk2`); warm-up; run the load generator on separate cores; repeat runs.
- **🔨 Build:** `bench/run.sh`, a repeatable benchmark suite: static response, proxy, cache hit, TLS.

### [ ] 11.2 Profiling
- **📖 Learn:** `perf record`/`perf stat` (Linux), Instruments (macOS), flamegraphs (Brendan Gregg), `strace -c` (syscall counts), CPU cache misses and branch mispredicts.
- **✅ Done when:** You've produced a flamegraph and identified the top 3 hotspots.

### [ ] 11.3 Reduce syscalls & copies
- **📖 Learn:** `writev` / `readv` (scatter-gather I/O); `sendfile`; `splice` (Linux zero-copy proxying); `TCP_NODELAY`; `TCP_CORK` / `TCP_NOPUSH`; `MSG_ZEROCOPY`.
- **🔨 Build:** Use `writev` for header + body; try `splice` for proxied bodies on Linux.

### [ ] 11.4 Memory optimization
- **📖 Learn:** Cost of `malloc`; **memory pools / arena allocators** (per-request arena freed in one go, like nginx's `ngx_pool_t`); slab allocators; object free-lists; jemalloc / mimalloc comparison.
- **🔨 Build:** Per-connection arena and a buffer free-list.
- **📏 Measure:** Allocations per request before and after.

### [ ] 11.5 Kernel & OS tuning
- **📖 Learn:** `somaxconn`, `tcp_max_syn_backlog`, ephemeral port range, `ulimit -n`, `SO_RCVBUF`/`SO_SNDBUF`, IRQ / NIC queue affinity.

### [ ] 11.6 Explore `io_uring` (stretch)
- **📖 Learn:** Completion-based I/O vs readiness-based I/O (epoll); submission / completion queues; why it reduces syscalls.
- **🧪 Experiment:** An `io_uring` backend for your event loop abstraction (Linux only).

### [ ] 11.7 Compare with nginx
- **✅ Done when:** `bench/RESULTS.md` has a table comparing your proxy and nginx (same hardware, same config) for each scenario, with analysis of where and why you're faster or slower.

---

## Phase 12: Production Hardening 🛡️

*Goal: make it robust, observable and safe.*

### [ ] 12.1 Logging
- **📖 Learn:** Why logging from the hot path is slow (locks + syscalls); async logging with a per-thread lock-free ring buffer + a logger thread; log levels; access log format.
- **🔨 Build:** `src/core/log.c` with an access log + an error log.

### [ ] 12.2 Metrics & observability
- **🔨 Build:** A `/__status` endpoint (or Prometheus text format) with req/s, latencies, cache hit ratio, upstream health and active connections.

### [ ] 12.3 Rate limiting
- **📖 Learn:** Token bucket vs leaky bucket vs sliding window; per-IP limits; doing it across worker threads (sharded maps + atomics).
- **🔨 Build:** Per-client-IP token bucket, returning `429 Too Many Requests`.

### [ ] 12.4 Security hardening
- **📖 Learn:** Slowloris & slow POST; request smuggling (revisit Phase 2); header injection; limits on everything (header size, body size, connections per IP); dropping privileges after binding port 443 (`setuid`); OWASP basics.
- **🔨 Build:** Enforce all limits; drop privileges; re-run the fuzzers against the full server.

### [ ] 12.5 Testing & CI
- **📖 Learn:** Unit vs integration vs end-to-end tests; GitHub Actions.
- **🔨 Build:** CI that builds on Linux + macOS, runs the unit tests, the integration tests (real backends + proxy + curl) and the ASan/TSan builds, and does a short fuzz run.
- **✅ Done when:** Every push runs the full suite automatically.

### [ ] 12.6 Documentation & retrospective
- **🔨 Build:** Update the README with real benchmark numbers, an architecture diagram and a config reference. Write `notes/RETROSPECTIVE.md`: what was hardest, what you'd redesign, what surprised you.

---

## 🚀 Beyond (after v1.0)
- [ ] HTTP/2 (binary framing, HPACK, stream multiplexing)
- [ ] WebSocket proxying (`Upgrade` handling)
- [ ] Response compression (gzip / brotli)
- [ ] TLS to upstreams (re-encryption) and mTLS
- [ ] HTTP/3 & QUIC (UDP-based transport)
- [ ] Distributed cache across proxy instances
- [ ] Lua / WASM scripting hooks

---

## 📝 Concept Index

Where each fundamental is learned:

| Concept | Phase / Task |
|---|---|
| Sockets, TCP/IP, byte order | 1.1 – 1.3 |
| State machines, incremental parsing | 2.2 – 2.3 |
| Fuzzing | 2.5, 12.4 |
| Processes vs threads, `fork` | 3.1 |
| **Race conditions** | 3.2 |
| **Mutexes, critical sections** | 3.3 |
| **Deadlock, livelock, starvation** | 3.4 |
| **Condition variables** | 3.5 |
| **Semaphores** | 3.6 |
| **Producer–consumer, thread pools** | 3.7 |
| Reader–writer locks | 3.8, 9.3 |
| Non-blocking I/O, `epoll` / `kqueue` | 4.1 – 4.3 |
| Reactor pattern, callbacks | 4.4 |
| Timers: heaps, timer wheels | 4.6 |
| Atomics, memory ordering, CAS | 5.3 |
| False sharing, CPU caches | 5.4 |
| Signals | 5.5 |
| Back-pressure, streaming | 6.4 |
| Connection pooling | 6.5 |
| Lexers / parsers, tries | 7.1 – 7.2 |
| RCU / safe memory reclamation | 7.3 |
| Hashing, consistent hashing | 8.4 |
| Hash tables, linked lists, LRU | 9.1 |
| Reference counting | 9.3 |
| Cache stampede, request coalescing | 9.4 |
| Cryptography, TLS | 10.1 – 10.6 |
| Profiling, benchmarking | 11.1 – 11.2 |
| Zero-copy I/O | 11.3 |
| Memory pools / arenas | 11.4 |
| Rate limiting | 12.3 |
