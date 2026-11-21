#!/usr/bin/env bash
# bench/run.sh - repeatable benchmark suite for aproxy.
#
#   ./bench/run.sh                 full suite (~6 min)
#   ./bench/run.sh --quick         short runs (~1.5 min), for a smoke test
#   ./bench/run.sh --only h1-hit,h2-hit,sweep-conns
#   ./bench/run.sh --list          show scenario names
#
# It builds a Release proxy, starts two fast C origin servers (bench/origin.c),
# starts the proxy with a generated config, runs each scenario with wrk / h2load,
# and writes everything to bench/results/<timestamp>/:
#   raw/*.txt        untouched tool output (the source of truth)
#   manifest.tsv     what was run, with which parameters
#   results.json     parsed numbers
#   RESULTS.md       tables + highlights; also copied to bench/RESULTS.md
#   charts/*.svg     charts; also copied to bench/charts/
#
# Tunables (environment variables):
#   DURATION=15  WARMUP=3       seconds per measured / warm-up run
#   WRK_THREADS=4               load-generator threads
#   CONNS=256                   connections for the fixed-concurrency scenarios
#   PROXY_WORKERS=N             proxy worker threads (default: cores - WRK_THREADS)
#   SWEEP_CONNS="16 64 256 1024 4096"
#   SWEEP_WORKERS="1 2 4 ..."   (default: powers of two up to PROXY_WORKERS)
#   H2_STREAMS=32               concurrent streams per HTTP/2 / HTTP/3 connection
#   HTTP_PORT=18080 TLS_PORT=18443 ORIGIN1=19101 ORIGIN2=19102
#   PIN=1                       Linux only: pin proxy and load generator to
#                               separate CPU sets with taskset
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
cd "$ROOT"

ALL_SCENARIOS="origin h1-hit h1-miss h1-tls-hit h2-hit h3-hit h1-newconn tls-handshake payload sweep-conns sweep-workers"

# ---------------------------------------------------------------- arguments
ONLY=""
NO_BUILD=0
QUICK=0
while [ $# -gt 0 ]; do
    case "$1" in
    --quick) QUICK=1 ;;
    --only) ONLY="${2//,/ }"; shift ;;
    --no-build) NO_BUILD=1 ;;
    --list) echo "$ALL_SCENARIOS" | tr ' ' '\n'; exit 0 ;;
    -h | --help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
    esac
    shift
done

ncpu() { getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4; }
NCPU=$(ncpu)

if [ "$QUICK" = 1 ]; then
    DURATION=${DURATION:-5}
    WARMUP=${WARMUP:-1}
    SWEEP_CONNS=${SWEEP_CONNS:-"64 512 2048"}
else
    DURATION=${DURATION:-15}
    WARMUP=${WARMUP:-3}
    SWEEP_CONNS=${SWEEP_CONNS:-"16 64 256 1024 4096"}
fi
WRK_THREADS=${WRK_THREADS:-4}
CONNS=${CONNS:-256}
H2_STREAMS=${H2_STREAMS:-32}
HTTP_PORT=${HTTP_PORT:-18080}
TLS_PORT=${TLS_PORT:-18443}
ORIGIN1=${ORIGIN1:-19101}
ORIGIN2=${ORIGIN2:-19102}
DEFAULT_WORKERS=$((NCPU - WRK_THREADS))
[ "$DEFAULT_WORKERS" -lt 1 ] && DEFAULT_WORKERS=1
PROXY_WORKERS=${PROXY_WORKERS:-$DEFAULT_WORKERS}
if [ -z "${SWEEP_WORKERS:-}" ]; then
    SWEEP_WORKERS=""
    w=1
    while [ "$w" -lt "$PROXY_WORKERS" ]; do SWEEP_WORKERS="$SWEEP_WORKERS $w"; w=$((w * 2)); done
    SWEEP_WORKERS="$SWEEP_WORKERS $PROXY_WORKERS"
fi

want() { [ -z "$ONLY" ] && return 0; case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }

# ---------------------------------------------------------------- CPU pinning (Linux)
SERVER_CPUS=""
LOADGEN_CPUS=""
if [ "${PIN:-0}" = 1 ] && command -v taskset >/dev/null; then
    half=$((NCPU / 2))
    SERVER_CPUS="0-$((half - 1))"
    LOADGEN_CPUS="$half-$((NCPU - 1))"
    [ "$PROXY_WORKERS" -gt "$half" ] && PROXY_WORKERS=$half
fi
# Only call in the background (cmd &): exec keeps $! equal to the server PID.
srv() { if [ -n "$SERVER_CPUS" ]; then exec taskset -c "$SERVER_CPUS" "$@"; else exec "$@"; fi; }
lg() { if [ -n "$LOADGEN_CPUS" ]; then taskset -c "$LOADGEN_CPUS" "$@"; else "$@"; fi; }

# ---------------------------------------------------------------- tools
need() { command -v "$1" >/dev/null || { echo "error: '$1' not found. $2" >&2; exit 1; }; }
need wrk "Install it: brew install wrk | apt install wrk"
need curl ""
need python3 ""
HAVE_H2LOAD=0
command -v h2load >/dev/null && HAVE_H2LOAD=1
[ "$HAVE_H2LOAD" = 0 ] && echo "note: h2load not found (brew install nghttp2 | apt install nghttp2-client); HTTP/2 and HTTP/3 scenarios are skipped" >&2

# A small accept backlog makes connection bursts fail with connect errors
# (macOS defaults to 128), which would show up in the high-concurrency runs.
if [ "$(uname)" = Darwin ]; then
    SOMAXCONN=$(sysctl -n kern.ipc.somaxconn 2>/dev/null || echo 0)
    TUNE_HINT="sudo sysctl -w kern.ipc.somaxconn=4096"
else
    SOMAXCONN=$(cat /proc/sys/net/core/somaxconn 2>/dev/null || echo 4096)
    TUNE_HINT="sudo sysctl -w net.core.somaxconn=4096"
fi
if [ "$SOMAXCONN" -lt 1024 ]; then
    echo "note: listen backlog limit (somaxconn) is $SOMAXCONN; runs above ~1000 connections" >&2
    echo "      may report connect errors. To raise it until reboot: $TUNE_HINT" >&2
fi
if [ "$(ulimit -n)" != unlimited ] && [ "$(ulimit -n)" -lt 16384 ]; then
    ulimit -n 65536 2>/dev/null || echo "note: ulimit -n is $(ulimit -n); raise it for the 4096-connection run" >&2
fi

# ---------------------------------------------------------------- build
if [ "$NO_BUILD" = 0 ]; then
    echo "==> building Release proxy"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build -j "$NCPU" --target proxy >/dev/null
fi
[ -x build/proxy ] || { echo "error: build/proxy missing (drop --no-build)" >&2; exit 1; }
echo "==> building bench origin"
cc -O2 -pthread "$BENCH/origin.c" -o build/bench-origin
[ -f certs/server.crt ] || ./scripts/gen-certs.sh >/dev/null

# HTTP/3 needs a proxy built with it; h2load's QUIC support is probed once the
# proxy is up (many packaged h2load builds accept --h3 but speak TCP only).
PROXY_H3=0
build/proxy -v | grep -q "http/3: yes" && PROXY_H3=1
HAVE_H3=0

# ---------------------------------------------------------------- output dir
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$BENCH/results/$STAMP"
RAW="$OUT/raw"
mkdir -p "$RAW"
MANIFEST="$OUT/manifest.tsv"
printf 'name\tgroup\ttool\tproto\tparam\tconns\tthreads\tstreams\tworkers\turl\n' >"$MANIFEST"

# Machine / build description for the report.
{
    echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo -dirty)"
    echo "os=$(uname -sr)"
    echo "arch=$(uname -m)"
    if [ "$(uname)" = Darwin ]; then
        echo "cpu=$(sysctl -n machdep.cpu.brand_string)"
        echo "mem_gb=$(($(sysctl -n hw.memsize) / 1073741824))"
    else
        echo "cpu=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ //' || uname -m)"
        echo "mem_gb=$(($(grep MemTotal /proc/meminfo | awk '{print $2}') / 1048576))"
    fi
    echo "cores=$NCPU"
    echo "proxy=$(build/proxy -v)"
    echo "compiler=$(cc --version | head -1)"
    echo "wrk=$(wrk -v 2>&1 | head -1 | cut -d' ' -f1-3)"
    [ "$HAVE_H2LOAD" = 1 ] && echo "h2load=$(h2load --version | head -1)"
    echo "duration_s=$DURATION"
    echo "warmup_s=$WARMUP"
    echo "wrk_threads=$WRK_THREADS"
    echo "proxy_workers=$PROXY_WORKERS"
    echo "pinned=${PIN:-0}"
    echo "somaxconn=$SOMAXCONN"
} >"$OUT/env.txt"

# ---------------------------------------------------------------- processes
PIDS=()
PROXY_PID=""
cleanup() {
    [ -n "$PROXY_PID" ] && kill "$PROXY_PID" 2>/dev/null || true
    for p in ${PIDS[@]+"${PIDS[@]}"}; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

port_free() { ! (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; }
for p in "$HTTP_PORT" "$TLS_PORT" "$ORIGIN1" "$ORIGIN2"; do
    port_free "$p" || { echo "error: port $p is in use (override with HTTP_PORT/TLS_PORT/ORIGIN1/ORIGIN2)" >&2; exit 1; }
done

echo "==> starting origins on :$ORIGIN1 and :$ORIGIN2"
srv build/bench-origin "$ORIGIN1" origin-A 2>"$RAW/origin-A.log" &
PIDS+=($!)
srv build/bench-origin "$ORIGIN2" origin-B 2>"$RAW/origin-B.log" &
PIDS+=($!)

wait_http() { # url, timeout seconds
    local i=0
    until curl -sk -o /dev/null --max-time 1 "$1"; do
        i=$((i + 1))
        [ "$i" -gt $(($2 * 10)) ] && return 1
        sleep 0.1
    done
}
wait_http "http://127.0.0.1:$ORIGIN1/health" 5 || { echo "origin failed to start" >&2; exit 1; }

CUR_WORKERS=""
start_proxy() { # workers
    [ "$CUR_WORKERS" = "$1" ] && return 0
    stop_proxy
    local h3=""
    [ "$PROXY_H3" = 1 ] && h3="http3 $TLS_PORT;"
    sed -e "s|@WORKERS@|$1|" -e "s|@HTTP_PORT@|$HTTP_PORT|" -e "s|@TLS_PORT@|$TLS_PORT|" \
        -e "s|@H3_LINE@|$h3|" -e "s|@ROOT@|$ROOT|" -e "s|@ORIGIN1@|$ORIGIN1|" \
        -e "s|@ORIGIN2@|$ORIGIN2|" "$BENCH/lib/proxy.conf.in" >"$OUT/proxy-w$1.conf"
    srv build/proxy -c "$OUT/proxy-w$1.conf" 2>>"$RAW/proxy.log" &
    PROXY_PID=$!
    wait_http "http://127.0.0.1:$HTTP_PORT/__status" 10 || { echo "proxy failed to start; see $RAW/proxy.log" >&2; exit 1; }
    # Upstreams start out unknown until the first health check passes (503 until then).
    local i=0
    until [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$HTTP_PORT/api/ready")" = 200 ]; do
        i=$((i + 1))
        [ "$i" -gt 150 ] && { echo "upstreams never became healthy; see $RAW/proxy.log" >&2; exit 1; }
        sleep 0.1
    done
    # Prime the cache for the cache-hit scenarios.
    for path in / /bytes/1024 /bytes/16384 /bytes/262144; do
        curl -s -o /dev/null "http://127.0.0.1:$HTTP_PORT$path"
    done
    CUR_WORKERS=$1
    echo "==> proxy running with $1 worker(s) (pid $PROXY_PID)"
    if [ "$PROXY_H3" = 1 ] && [ "$HAVE_H2LOAD" = 1 ] && [ "$HAVE_H3" = 0 ] &&
        h2load --h3 -n 20 -c 1 "https://127.0.0.1:$TLS_PORT/" 2>&1 | grep -q " 20 succeeded"; then
        HAVE_H3=1
    fi
}
stop_proxy() {
    if [ -n "$PROXY_PID" ]; then
        kill "$PROXY_PID" 2>/dev/null || true
        wait "$PROXY_PID" 2>/dev/null || true
        PROXY_PID=""
        CUR_WORKERS=""
        sleep 0.5
    fi
}

# Sample the proxy's resident memory every 0.5 s while a run is in progress.
RSS_PID=""
rss_start() {
    local pid=${PROXY_PID:-}
    [ -z "$pid" ] && return 0
    (while kill -0 "$pid" 2>/dev/null; do ps -o rss= -p "$pid" 2>/dev/null; sleep 0.5; done) >"$1" &
    RSS_PID=$!
}
rss_stop() { [ -n "$RSS_PID" ] && kill "$RSS_PID" 2>/dev/null; wait "$RSS_PID" 2>/dev/null || true; RSS_PID=""; }

record() { # name group tool proto param conns threads streams workers url
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >>"$MANIFEST"
}

# ---------------------------------------------------------------- runners
run_wrk() { # name group proto param conns url [header]
    local name=$1 group=$2 proto=$3 param=$4 conns=$5 url=$6 header=${7:-}
    local threads=$WRK_THREADS
    [ "$conns" -lt "$threads" ] && threads=$conns
    printf '  %-28s wrk  c=%-5s t=%s %s\n' "$name" "$conns" "$threads" "$url"
    if [ "$WARMUP" -gt 0 ]; then
        lg env WRK_HEADER="$header" wrk -t"$threads" -c"$conns" -d"${WARMUP}s" \
            -s "$BENCH/lib/report.lua" "$url" >/dev/null 2>&1 || true
    fi
    rss_start "$RAW/$name.rss"
    lg env WRK_HEADER="$header" wrk -t"$threads" -c"$conns" -d"${DURATION}s" \
        --timeout 10s --latency -s "$BENCH/lib/report.lua" "$url" >"$RAW/$name.txt" 2>&1 || true
    rss_stop
    record "$name" "$group" wrk "$proto" "$param" "$conns" "$threads" 1 "${CUR_WORKERS:--}" "$url"
}

run_h2load() { # name group proto param conns url [extra h2load args...]
    local name=$1 group=$2 proto=$3 param=$4 conns=$5 url=$6
    shift 6
    local threads=$WRK_THREADS
    [ "$conns" -lt "$threads" ] && threads=$conns
    printf '  %-28s h2load c=%-5s m=%s t=%s %s %s\n' "$name" "$conns" "$H2_STREAMS" "$threads" "$*" "$url"
    rss_start "$RAW/$name.rss"
    lg h2load -c "$conns" -m "$H2_STREAMS" -t "$threads" \
        --warm-up-time="${WARMUP}s" -D "${DURATION}s" "$@" "$url" >"$RAW/$name.txt" 2>&1 || true
    rss_stop
    record "$name" "$group" h2load "$proto" "$param" "$conns" "$threads" "$H2_STREAMS" "${CUR_WORKERS:--}" "$url"
}

H="http://127.0.0.1:$HTTP_PORT"
S="https://127.0.0.1:$TLS_PORT"

echo "==> results -> $OUT"
echo "==> duration ${DURATION}s, warm-up ${WARMUP}s, wrk threads $WRK_THREADS, proxy workers $PROXY_WORKERS, cores $NCPU"

# ---------------------------------------------------------------- scenarios
if want origin; then
    run_wrk origin baseline http/1.1 "direct to origin" "$CONNS" "http://127.0.0.1:$ORIGIN1/"
fi

start_proxy "$PROXY_WORKERS"
want h1-hit && run_wrk h1-hit main http/1.1 "cache hit" "$CONNS" "$H/"
want h1-miss && run_wrk h1-miss main http/1.1 "proxied, cache off" "$CONNS" "$H/api/item"
want h1-tls-hit && run_wrk h1-tls-hit main "http/1.1+tls" "cache hit" "$CONNS" "$S/"
if [ "$HAVE_H2LOAD" = 1 ]; then
    want h2-hit && run_h2load h2-hit main "h2" "cache hit" 64 "$S/"
    if want h3-hit; then
        if [ "$HAVE_H3" = 1 ]; then
            run_h2load h3-hit main "h3" "cache hit" 64 "$S/" --h3
        else
            echo "  h3-hit                       skipped: needs a proxy built with HTTP/3 and an h2load built with QUIC (ngtcp2)"
            echo "h3-hit: skipped, h2load without QUIC support or proxy without HTTP/3" >>"$OUT/skipped.txt"
        fi
    fi
fi
want h1-newconn && run_wrk h1-newconn conn http/1.1 "new connection per request" "$CONNS" "$H/" "Connection: close"
want tls-handshake && run_wrk tls-handshake conn "http/1.1+tls" "full TLS handshake per request" "$CONNS" "$S/" "Connection: close"

if want payload; then
    for size in 1024 16384 262144; do
        run_wrk "payload-$size" payload http/1.1 "$size" "$CONNS" "$H/bytes/$size"
    done
fi

if want sweep-conns; then
    for c in $SWEEP_CONNS; do
        run_wrk "conns-$c" sweep-conns http/1.1 "$c" "$c" "$H/"
    done
fi

# Worker scaling uses HTTP/2: over loopback a single worker already saturates what
# wrk can drive with HTTP/1.1 (one request per syscall round trip), so an HTTP/1.1
# sweep would measure the load generator. h2load multiplexes many streams per
# connection and keeps the proxy CPU-bound. Falls back to wrk without h2load.
if want sweep-workers; then
    for w in $SWEEP_WORKERS; do
        start_proxy "$w"
        if [ "$HAVE_H2LOAD" = 1 ]; then
            run_h2load "workers-$w" sweep-workers h2 "$w" 64 "$S/"
        else
            run_wrk "workers-$w" sweep-workers http/1.1 "$w" "$CONNS" "$H/"
        fi
    done
    start_proxy "$PROXY_WORKERS"
fi

curl -s "$H/__status" >"$OUT/status.json" || true
stop_proxy

# ---------------------------------------------------------------- report
echo "==> writing report"
python3 "$BENCH/report.py" "$OUT"
mkdir -p "$BENCH/charts"
cp "$OUT/RESULTS.md" "$BENCH/RESULTS.md"
cp "$OUT"/charts/*.svg "$BENCH/charts/" 2>/dev/null || true
echo
echo "Done. Summary: bench/RESULTS.md   Raw output: ${OUT#"$ROOT"/}"
