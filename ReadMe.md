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
| **HTTP/1.1, HTTP/2, HTTP/3** | One protocol-neutral request pipeline behind three frontends: a hand-written HTTP/1.1 parser, HTTP/2 on nghttp2 (negotiated by ALPN), and HTTP/3 over QUIC on ngtcp2 + nghttp3, advertised to TCP clients with `Alt-Svc`. |
| **Status endpoint** | `GET /__status` returns JSON counters: connections, requests, cache hits, per-upstream health. |

### Stretch goals
Rate limiting (token bucket), async logging, graceful reload, `sendfile` zero-copy static files, WebSocket proxying, response compression, TLS to upstreams.

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
  ├── accept()  (every worker polls the shared listening sockets; the kernel
  │              wakes one of them per connection, losers get EAGAIN)
  ├── HTTP/3 endpoint (worker 0 only: QUIC has no accept(), one UDP socket
  │              receives every connection's packets, demultiplexed by connection ID)
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

## 📁 Layout

```
.
├── src/
│   ├── main.c
│   ├── core/        # event loop, buffers, timers, memory pools, logging
│   ├── net/         # sockets, listeners, connection handling
│   ├── http/        # HTTP/1.1 parser, HTTP/2 and HTTP/3 frontends, request/response model
│   ├── proxy/       # upstream, connection pool, forwarding
│   ├── lb/          # load-balancing algorithms, health checks
│   ├── cache/       # LRU cache, cache policy
│   ├── tls/         # OpenSSL integration
│   └── config/      # config file parser
├── config/          # proxy.conf: the example configuration used below
├── scripts/         # gen-certs.sh, demo backends (backend.py, run-backends.sh)
├── tests/           # unit tests (ctest)
├── bench/           # benchmark scripts and results
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
- **TLS:** OpenSSL 3.x / 4.x (QUIC needs OpenSSL 3.5 or newer)
- **HTTP/2:** nghttp2 (optional)
- **HTTP/3:** ngtcp2 + nghttp3 with ngtcp2's OpenSSL crypto backend (optional)
- **Tooling:** `lldb`/`gdb`, AddressSanitizer / ThreadSanitizer / UBSan, Valgrind (Linux), `wrk` / `h2load` / `ab` for load tests, `perf` / Instruments with flamegraphs for profiling
- **Runtime dependencies:** OpenSSL, plus nghttp2 / ngtcp2 / nghttp3 only when built with HTTP/2 / HTTP/3.

## 🚀 Build & Run

The proxy is plain C11 on POSIX. It runs natively on **macOS, Linux and the BSDs**
(`kqueue` or `epoll`). On **Windows** use WSL 2 and follow the Linux steps; there is no
native Windows build because the event loop needs `epoll`/`kqueue` and the code uses
`pthreads`.

### 1. Requirements

| What | Version | Needed for |
|---|---|---|
| C compiler | clang or gcc with C11 | everything |
| CMake | 3.16 or newer | everything |
| OpenSSL | 3.0 or newer (3.5+ for HTTP/3) | TLS termination |
| nghttp2 | any recent | HTTP/2 (optional) |
| ngtcp2 + nghttp3 | ngtcp2 1.12+ built with its OpenSSL backend (`libngtcp2_crypto_ossl`) | HTTP/3 (optional) |
| Python 3 | any | the demo backends only |
| curl | 7.x+; a QUIC-enabled build for HTTP/3 tests | trying it out |

HTTP/2 and HTTP/3 are **optional**: CMake detects the libraries and silently builds a
proxy without those frontends if they are missing (you will see a `HTTP/3: ... frontend
disabled` warning at configure time). Everything else works the same.

#### Install the dependencies

**macOS (Homebrew)**

```bash
brew install cmake openssl@4 libnghttp2 libngtcp2 libnghttp3 curl
```

`libngtcp2` pulls in a QUIC-capable OpenSSL. The Homebrew `curl` is only needed for
HTTP/3 testing (Apple's `/usr/bin/curl` has no QUIC support); call it as
`/opt/homebrew/opt/curl/bin/curl` or put `$(brew --prefix curl)/bin` first in `PATH`.

**Debian / Ubuntu**

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libssl-dev libnghttp2-dev \
                 libnghttp3-dev libngtcp2-dev python3 curl
```

Most distributions do not yet ship ngtcp2's OpenSSL crypto backend (it needs OpenSSL
3.5+), so this gives you HTTP/1.1 + HTTP/2 with HTTP/3 disabled. To get HTTP/3 on
Linux, build [nghttp3](https://github.com/ngtcp2/nghttp3) and
[ngtcp2](https://github.com/ngtcp2/ngtcp2) from source against OpenSSL 3.5+ with the
OpenSSL backend enabled (see ngtcp2's README), then re-run CMake.

**Fedora / RHEL**

```bash
sudo dnf install gcc cmake openssl-devel libnghttp2-devel nghttp3-devel ngtcp2-devel python3 curl
```

**Arch Linux**

```bash
sudo pacman -S base-devel cmake openssl libnghttp2 libnghttp3 libngtcp2 python curl
```

**Alpine**

```bash
sudo apk add build-base cmake openssl-dev nghttp2-dev nghttp3-dev ngtcp2-dev python3 curl
```

**FreeBSD**

```bash
sudo pkg install cmake openssl libnghttp2 libnghttp3 libngtcp2 python3 curl
```

**Windows**: install WSL 2 with Ubuntu (`wsl --install`), open the Ubuntu shell and
follow the Debian / Ubuntu steps. Ports opened inside WSL are reachable from Windows
at `localhost`.

### 2. Build

```bash
git clone <this repository> && cd MultiThreaded-Proxy-Server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/proxy -v          # aproxy 0.1.0 (http/2: yes, http/3: yes)
```

Configure-time options (pass as `-DNAME=VALUE` to the first `cmake` command):

| Option | Default | Meaning |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` builds with `-O0 -g` and, by default, ASan + UBSan |
| `ENABLE_HTTP2` | `ON` | build the HTTP/2 frontend if nghttp2 is found |
| `ENABLE_HTTP3` | `ON` | build the HTTP/3 frontend if ngtcp2 + nghttp3 are found |
| `ENABLE_SANITIZERS` | `ON` | AddressSanitizer + UBSan in Debug builds |
| `BUILD_TESTING` | `ON` | build the unit tests |
| `OPENSSL_ROOT_DIR` | auto | point CMake at a specific OpenSSL (e.g. `/usr/local/opt/openssl@3`) |

CMake also writes `build/compile_commands.json`; `.clangd` in the repo root points
clangd at it, and VS Code's C/C++ extension can be told to use it via
`C_Cpp.default.compileCommands` in `.vscode/settings.json`.

### 3. Run

The example configuration `config/proxy.conf` listens on **8080** (plain HTTP/1.1) and
**8443** (TLS: HTTP/1.1 and HTTP/2 by ALPN, plus HTTP/3 over QUIC on UDP 8443) and
load-balances between two demo backends on 9001 and 9002.

```bash
# 1. A self-signed certificate for localhost (writes certs/server.crt and .key)
./scripts/gen-certs.sh

# 2. Two demo origin servers (Ctrl-C stops both); or run them in another terminal
./scripts/run-backends.sh &

# 3. Check the configuration, then start the proxy (Ctrl-C or SIGTERM stops it cleanly)
./build/proxy -t -c config/proxy.conf
./build/proxy -c config/proxy.conf
```

Command line: `-c FILE` configuration file (default `config/proxy.conf`), `-t` test the
configuration and exit, `-v` print the version and which protocols were built in.

Try it from another terminal:

```bash
curl http://localhost:8080/                                   # HTTP/1.1, plaintext
curl -k --http1.1 -I https://localhost:8443/                  # HTTP/1.1 over TLS
curl -k --http2 -I https://localhost:8443/                    # HTTP/2 (ALPN)
curl -k --http3-only -I https://localhost:8443/               # HTTP/3 (needs a QUIC-enabled curl)
curl -k --alt-svc /tmp/altsvc https://localhost:8443/ -o /dev/null -w '%{http_version}\n'
curl -k --alt-svc /tmp/altsvc https://localhost:8443/ -o /dev/null -w '%{http_version}\n'   # 3: switched via Alt-Svc
curl http://localhost:8080/__status                           # JSON counters and upstream health
curl -X POST --data 'hello' http://localhost:8080/api/echo    # uncached route, body forwarded
```

`-k` is needed because the certificate is self-signed. One access-log line per request
is written to stderr; set `log_level debug;` in the config to see connection-level
detail.

### 4. Configure

Edit `config/proxy.conf`; the syntax is nginx-like and every directive is commented in
that file. The main ones:

```
workers 0;                       # 0 = one worker per CPU core
log_level info;                  # debug | info | warn | error
listen 8080;                     # plaintext HTTP/1.1
listen 8443 ssl;                 # TLS; HTTP/1.1 and HTTP/2 chosen by ALPN
http3 8443;                      # HTTP/3 over QUIC (UDP); advertised with Alt-Svc
ssl_certificate     certs/server.crt;
ssl_certificate_key certs/server.key;
tls_host example.com { ssl_certificate ...; ssl_certificate_key ...; }   # per-SNI certs
max_header_size 16k;  max_body_size 8m;  header_timeout 10s;  idle_timeout 60s;

upstream backend {
    strategy least_conn;         # round_robin | weighted | least_conn | ip_hash
    server 127.0.0.1:9001 weight=3;
    server 127.0.0.1:9002 max_fails=3 fail_timeout=10s;
    health_check interval=5s path=/health;
    connect_timeout 2s;  read_timeout 30s;  keepalive 32;
}
cache { max_size 256m; default_ttl 60s; shards 16; }
route /    { proxy_pass backend; cache on; }
route /api { proxy_pass backend; cache off; }
```

To listen on 80/443 run as root or grant the binary `CAP_NET_BIND_SERVICE` on Linux
(`sudo setcap cap_net_bind_service=+ep build/proxy`). Privilege dropping is not
implemented yet, so prefer a high port behind a firewall rule for anything exposed.

### 5. Tests and debug builds

```bash
ctest --test-dir build --output-on-failure          # unit tests

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug   # ASan + UBSan on by default
cmake --build build-debug -j
./build-debug/proxy -c config/proxy.conf            # sanitizer reports go to stderr
```

### 6. Troubleshooting

| Symptom | Fix |
|---|---|
| `Could NOT find OpenSSL` | Install the dev package (`libssl-dev`, `openssl-devel`) or pass `-DOPENSSL_ROOT_DIR=/path/to/openssl`. |
| `HTTP/3: ngtcp2/nghttp3 not found, frontend disabled` | Expected on most Linux distros; see the Linux notes above. The proxy still serves HTTP/1.1 and HTTP/2. `http3 8443;` in the config is then ignored with a warning. |
| `bind 0.0.0.0:8080: Address already in use` | Something else owns the port (`lsof -i :8080` / `ss -ltnp`). Change `listen` or stop the other process. |
| `ssl_certificate ... requires ...` / cannot load certificate | Run `./scripts/gen-certs.sh` first; paths in the config are relative to the directory you start the proxy from. |
| `curl: option --http3-only: the installed libcurl version doesn't support this` | Your curl has no QUIC support. On macOS use Homebrew's curl (`/opt/homebrew/opt/curl/bin/curl`); on Linux install a QUIC-enabled build or test with `--http2`. |
| `502` / `upstream ... marked down` | The backends are not running or are unreachable; start `./scripts/run-backends.sh` and check `/__status`. |
| Editor shows errors the compiler does not | Point your language server at `build/compile_commands.json` (see the end of section 2). |

## 📊 Performance Goals

| Milestone | Target (on a laptop, keep-alive, small responses) |
|---|---|
| Blocking server | Baseline number; it's meant to be slow |
| Thread pool | ~10× the baseline |
| Single event loop | 50k+ req/s |
| Multi-reactor | 150k+ req/s, scaling with cores |
| Proxy + cache hit | Within ~20% of nginx with the same config |

Every result goes into `bench/RESULTS.md` along with the hardware and command used.

### Running the benchmarks

```bash
brew install wrk nghttp2          # or: sudo apt install wrk nghttp2-client
sudo sysctl -w kern.ipc.somaxconn=4096   # macOS; Linux: net.core.somaxconn (resets on reboot)
./bench/run.sh                    # full suite (~7 min); --quick for a 2-minute smoke test
```

The suite covers HTTP/1.1 cache hits and misses, HTTPS, HTTP/2, HTTP/3, TLS handshakes
per second, response sizes, a 16–4096 connection sweep and multi-core scaling. It
writes [`bench/RESULTS.md`](./bench/RESULTS.md) (tables, charts, highlights) and keeps
raw tool output in `bench/results/`. See [`bench/README.md`](./bench/README.md) for the
scenarios, tuning knobs and methodology.

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
