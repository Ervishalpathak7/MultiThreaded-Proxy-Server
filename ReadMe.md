# ⚡ Async Reverse Proxy in C

> A high-performance, event-driven web server and reverse proxy written from scratch in C.
> It is a **build-to-learn** project: every feature starts with studying the concepts behind it, then building it yourself.

---

## 🎯 Vision

The goal is a small, very fast **asynchronous** web server, in the same family as nginx, HAProxy or Envoy. Each part of it is written by hand, so that by the end you understand every layer:

```
            ┌──────────────────────────────────────────────┐
 Client ──► │  TLS termination ─► HTTP parser ─► Router    │
 (HTTPS)    │                                   │          │
            │                       ┌── LRU Cache (hit) ──►│──► Response
            │                       │                      │
            │                       └── Load Balancer ─────│──► Backend 1
            │                                              │──► Backend 2
            │       Event loop (epoll / kqueue) × N cores  │──► Backend N
            └──────────────────────────────────────────────┘
```

## ✨ Core Features

| Feature | What it means |
|---|---|
| **Async I/O engine** | Non-blocking sockets with `epoll` (Linux) and `kqueue` (macOS/BSD), one event loop per CPU core (multi-reactor). No thread per connection. |
| **Reverse proxy** | Forwards client requests to upstream servers, rewrites headers (`X-Forwarded-For`, `Host`, hop-by-hop headers), streams bodies, and reuses upstream connections from a pool. |
| **Load balancing** | Round robin, weighted round robin, least connections, and consistent hashing, with active and passive health checks. |
| **LRU caching** | In-memory response cache built from a hash map and a doubly linked list. It follows HTTP caching rules (`Cache-Control`, `ETag`, TTL) and is sharded so threads don't block each other. |
| **SSL/TLS termination** | HTTPS on the front end using OpenSSL with non-blocking handshakes, SNI (one certificate per host) and session resumption. |

### Stretch goals
Rate limiting (token bucket), async logging, metrics endpoint, graceful reload, `sendfile` zero-copy static files, and HTTP/2.

## 🧠 Learning Philosophy

**Study → Build → Break → Measure → Improve**

1. **Study** the concepts each task needs. Every task in [`TASKS.md`](./TASKS.md) lists them.
2. **Build** the smallest working version yourself before looking at reference code.
3. **Break** it with load tests, sanitizers, fuzzing and bad input.
4. **Measure** it with benchmarks and profilers.
5. **Improve** it, and record what you learned in `notes/`.

Fundamentals covered along the way: TCP/IP and sockets, processes vs threads, **race conditions, mutexes, semaphores, condition variables, deadlocks**, the producer–consumer problem, atomics and memory ordering, I/O multiplexing, state machines, data structures (hash maps, linked lists, heaps, timer wheels), memory management, cryptography basics, and performance engineering.

👉 **The full step-by-step roadmap is in [`TASKS.md`](./TASKS.md).**

## 🏗️ Target Architecture

```
main thread
  ├── parse config, bind listening sockets, set up TLS contexts
  ├── spawn N worker threads (N = CPU cores)
  └── handle signals (SIGINT/SIGTERM → graceful shutdown, SIGHUP → reload)

worker thread × N   (each owns one event loop; it shares nothing except the cache and stats)
  ├── accept()  (SO_REUSEPORT: the kernel spreads connections across workers)
  ├── per-connection state machine:
  │     TLS_HANDSHAKE → READ_REQUEST → PARSE → CACHE_LOOKUP
  │        → (hit)  WRITE_RESPONSE
  │        → (miss) PICK_UPSTREAM → CONNECT → FORWARD → STREAM_RESPONSE → CACHE_STORE
  ├── timers (idle / header / upstream timeouts)
  └── upstream connection pool (per worker, so no locks)

shared
  ├── LRU cache (sharded; mutex or rwlock per shard; ref-counted entries)
  ├── upstream health state (atomics)
  └── stats counters (per-thread, summed when read)
```

## 📁 Planned Layout

```
.
├── src/
│   ├── main.c
│   ├── core/        # event loop, buffers, timers, memory pools, logging
│   ├── net/         # sockets, listeners, connection handling
│   ├── http/        # parser, request/response, headers, chunked encoding
│   ├── proxy/       # upstream, connection pool, forwarding
│   ├── lb/          # load-balancing algorithms, health checks
│   ├── cache/       # LRU cache, cache policy
│   ├── tls/         # OpenSSL integration
│   └── config/      # config file parser
├── include/
├── tests/           # unit tests + integration tests
├── bench/           # benchmark scripts and results
├── learn/           # small standalone experiments for each concept
├── notes/           # your learning notes, one file per topic
├── CMakeLists.txt
├── ReadMe.md
└── TASKS.md
```

## 🛠️ Tech Stack

- **Language:** C11 (POSIX)
- **Build:** CMake (plus a simple Makefile at the start)
- **I/O:** `epoll` (Linux), `kqueue` (macOS), behind a small shared interface
- **Concurrency:** `pthreads`, C11 `<stdatomic.h>`
- **TLS:** OpenSSL 3.x
- **Tooling:** `lldb`/`gdb`, AddressSanitizer / ThreadSanitizer / UBSan, Valgrind (Linux), `wrk` / `h2load` / `ab` for load tests, `perf` / Instruments with flamegraphs for profiling
- **No external runtime dependencies** apart from OpenSSL.

## 🚀 Build & Run (once Phase 0 is done)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/proxy -c proxy.conf
```

Example config (target format):

```
listen 8443 ssl;
ssl_certificate     certs/server.crt;
ssl_certificate_key certs/server.key;

upstream backend {
    strategy least_conn;
    server 127.0.0.1:9001 weight=3;
    server 127.0.0.1:9002;
    health_check interval=5s path=/health;
}

cache {
    max_size 256m;
    default_ttl 60s;
}

route / {
    proxy_pass backend;
    cache on;
}
```

## 📊 Performance Goals

| Milestone | Target (on a laptop, keep-alive, small responses) |
|---|---|
| Blocking server | Baseline number; it's meant to be slow |
| Thread pool | ~10× the baseline |
| Single event loop | 50k+ req/s |
| Multi-reactor | 150k+ req/s, scaling with cores |
| Proxy + cache hit | Within ~20% of nginx with the same config |

Every result goes into `bench/RESULTS.md` along with the hardware and command used.

## 📚 Core Reading List

- *Beej's Guide to Network Programming*: sockets
- *UNIX Network Programming, Vol. 1* (W. Richard Stevens): the main networking reference
- *The Linux Programming Interface* (Michael Kerrisk): system calls, processes, threads, signals
- *Operating Systems: Three Easy Pieces* (OSTEP): concurrency chapters (free online)
- *The Little Book of Semaphores* (Allen Downey): synchronization puzzles (free)
- *The C10K Problem* (Dan Kegel): why async I/O exists
- RFC 9110 (HTTP semantics), RFC 9111 (HTTP caching), RFC 9112 (HTTP/1.1), RFC 8446 (TLS 1.3)
- nginx architecture: *"Inside NGINX: How We Designed for Performance & Scale"*

## 📜 License

MIT
