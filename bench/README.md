# Benchmarks

A repeatable benchmark suite for the proxy. One command builds a Release binary,
starts fast origin servers, runs every scenario with `wrk` and `h2load`, and writes a
Markdown report with charts.

```bash
./bench/run.sh              # full suite, about 7 minutes
./bench/run.sh --quick      # 5-second runs, about 2 minutes (smoke test)
./bench/run.sh --only h1-hit,h2-hit,sweep-conns
./bench/run.sh --list       # scenario names
```

The latest summary is written to [`bench/RESULTS.md`](./RESULTS.md) with charts in
`bench/charts/`. Each run also keeps everything under `bench/results/<timestamp>/`
(ignored by git):

| File | Contents |
|---|---|
| `raw/*.txt` | unmodified wrk / h2load output, the source of every number |
| `raw/*.rss` | proxy resident memory sampled every 0.5 s during each run |
| `raw/proxy.log` | proxy stderr (warnings only; the access log is off) |
| `manifest.tsv` | each scenario's tool, URL, connections, threads, streams and workers |
| `env.txt` | hardware, OS, compiler, tool versions, commit |
| `proxy-w*.conf` | the exact proxy config used |
| `status.json` | `GET /__status` at the end (cache hit ratio, upstream errors) |
| `results.json`, `RESULTS.md`, `charts/` | parsed results and the report |

To regenerate a report from raw output: `python3 bench/report.py bench/results/<timestamp>`.

## Requirements

| Tool | Install | Used for |
|---|---|---|
| `wrk` | `brew install wrk` / `apt install wrk` | HTTP/1.1 and HTTPS scenarios, latency percentiles |
| `h2load` | `brew install nghttp2` / `apt install nghttp2-client` | HTTP/2, HTTP/3 and worker scaling (optional; skipped if missing) |
| `python3`, `curl`, a C compiler, CMake | already needed by the project | report, readiness checks, builds |

HTTP/3 also needs an `h2load` built with QUIC support (ngtcp2). Most packaged builds,
Homebrew's included, accept `--h3` but only speak TCP. The script probes for this and
records the scenario as skipped if the probe fails. To get it, build nghttp2 from source
with `--enable-http3` against the same ngtcp2/nghttp3 the proxy uses.

## Scenarios

| Name | Tool | What it measures |
|---|---|---|
| `origin` | wrk | the origin server directly, with no proxy (baseline) |
| `h1-hit` | wrk | HTTP/1.1 keep-alive, served from the LRU cache |
| `h1-miss` | wrk | HTTP/1.1 on a `cache off` route: parse, load-balance, pooled upstream connection, stream the response back |
| `h1-tls-hit` | wrk | HTTPS (TLS 1.3) keep-alive, cache hit: cost of encryption |
| `h2-hit` | h2load | HTTP/2 over TLS, 64 connections × 32 multiplexed streams |
| `h3-hit` | h2load | HTTP/3 over QUIC, same shape (needs a QUIC-enabled h2load) |
| `h1-newconn` | wrk | `Connection: close`: accept + parse + respond + close for every request |
| `tls-handshake` | wrk | `Connection: close` over TLS: full handshakes per second |
| `payload` | wrk | 1 KiB, 16 KiB and 256 KiB cached bodies: bytes per second |
| `sweep-conns` | wrk | cache-hit throughput and tail latency from 16 to 4096 connections |
| `sweep-workers` | h2load | throughput with 1, 2, 4 … N worker threads (multi-core scaling) |

Two origins (`bench/origin.c`, a minimal C server) sit behind the proxy, round-robin.
The demo `scripts/backend.py` is not used: it is Python and would bottleneck the proxied
scenario long before the proxy does.

## Tuning knobs

Pass these as environment variables, e.g. `DURATION=30 CONNS=512 ./bench/run.sh`:

| Variable | Default | Meaning |
|---|---|---|
| `DURATION` / `WARMUP` | 15 / 3 (quick: 5 / 1) | seconds per measured / discarded warm-up run |
| `CONNS` | 256 | connections for the fixed-concurrency HTTP/1.1 scenarios |
| `WRK_THREADS` | 4 | load-generator threads |
| `PROXY_WORKERS` | cores − `WRK_THREADS` | proxy workers, so the proxy and load generator do not fight over cores |
| `H2_STREAMS` | 32 | concurrent streams per HTTP/2 / HTTP/3 connection |
| `SWEEP_CONNS` | `16 64 256 1024 4096` | connection counts for `sweep-conns` |
| `SWEEP_WORKERS` | powers of two up to `PROXY_WORKERS` | worker counts for `sweep-workers` |
| `PIN=1` | off | Linux: pin the proxy/origins and the load generator to separate halves of the CPUs with `taskset` |
| `HTTP_PORT` `TLS_PORT` `ORIGIN1` `ORIGIN2` | 18080 18443 19101 19102 | ports, if those are taken |

## Getting trustworthy numbers

- **Raise the accept backlog.** macOS defaults `kern.ipc.somaxconn` to 128, so bursts of new
  connections get dropped, which shows up as connect errors at 1024+ connections and in
  `h1-newconn`. Before a run: `sudo sysctl -w kern.ipc.somaxconn=4096` (Linux:
  `sudo sysctl -w net.core.somaxconn=4096`). This setting resets on reboot. The script warns when it is low.
- **Plug in the laptop** and close heavy apps. Thermal throttling and background work
  move numbers by 10–20%.
- **Linux gives better numbers.** Run with `PIN=1` on a machine with 8+ cores, or best of all,
  put the load generator on a second machine. Point the tools at it by hand using
  the commands in `raw/*.txt` and `manifest.tsv`.
- **Run two or three times** and quote the median, not the best run.
- **Know what each row measures.** Over loopback, HTTP/1.1 with one request in flight
  per connection hits a ceiling of roughly one syscall round trip per request, so one
  proxy worker can already saturate wrk. That is why multi-core scaling is measured with
  HTTP/2, where streams are multiplexed and the proxy CPU is the bottleneck.
- wrk is closed-loop, so its p99.9/max suffer from coordinated omission. For
  latency-under-fixed-load numbers use [wrk2](https://github.com/giltene/wrk2)
  (`wrk2 -R 100000 ...`) on Linux.

## Putting the numbers on a resume

`RESULTS.md` opens with a **Highlights** list and a ready-made one-line summary
built from the actual run. Some tips:

- Quote the metric along with its conditions: *"180k req/s at p99 < 3 ms with 256
  concurrent keep-alive connections, HTTP/1.1 cache hits, 10-core Apple M5"*.
  A number without conditions doesn't mean much.
- Pair a throughput figure with a latency or efficiency figure (p99, RSS, scaling factor).
- Link the repository's `bench/RESULTS.md` so a reviewer can see the method and re-run it.
- Only quote numbers you can reproduce with `./bench/run.sh`.
