#!/usr/bin/env python3
"""Turn one bench/run.sh result directory into results.json, RESULTS.md and SVG charts.

    python3 bench/report.py bench/results/<timestamp>

Only the standard library is used. The raw tool output in <dir>/raw/ is the
source of truth; this script can be re-run on it at any time.
"""
import csv
import json
import re
import sys
from pathlib import Path

# ----------------------------------------------------------------- parsing

UNIT_US = {"us": 1.0, "ms": 1e3, "s": 1e6}


def to_us(text):
    m = re.fullmatch(r"([\d.]+)(us|ms|s)", text.strip())
    return float(m.group(1)) * UNIT_US[m.group(2)] if m else None


def parse_wrk(text):
    line = next((l for l in text.splitlines() if l.startswith("JSON:")), None)
    if not line:
        return None
    d = json.loads(line[5:])
    errors = d["http_errors"]
    ok = max(d["requests"] - d["http_errors"], 0)
    return {
        "requests": d["requests"],
        "ok": ok,
        "errors": errors,
        "socket_errors": d["err_connect"] + d["err_read"] + d["err_write"] + d["err_timeout"],
        "rps": ok / d["duration_s"] if d["duration_s"] else 0.0,
        "mbps": d["bytes"] / d["duration_s"] / 1e6 if d["duration_s"] else 0.0,
        "mean_us": d["lat_mean_us"],
        "p50_us": d["p50_us"],
        "p90_us": d["p90_us"],
        "p99_us": d["p99_us"],
        "p999_us": d["p999_us"],
        "max_us": d["lat_max_us"],
    }


def parse_h2load(text):
    fin = re.search(r"finished in ([\d.]+)s, ([\d.]+) req/s, ([\d.]+)([KMG]?B)/s", text)
    req = re.search(r"requests: (\d+) total, \d+ started, (\d+) done, (\d+) succeeded, (\d+) failed, "
                    r"(\d+) errored, (\d+) timeout", text)
    if not fin or not req:
        return None
    secs = float(fin.group(1))
    total, done, ok, failed, errored, timeout = map(int, req.groups())
    unit = {"B": 1, "KB": 1 << 10, "MB": 1 << 20, "GB": 1 << 30}[fin.group(4)]  # h2load uses binary units
    out = {
        "requests": done,
        "ok": ok,
        "errors": failed + timeout,
        "socket_errors": errored,
        "rps": ok / secs if secs else 0.0,
        "mbps": float(fin.group(3)) * unit / 1e6,
        "mean_us": None, "p50_us": None, "p90_us": None, "p99_us": None, "p999_us": None,
        "max_us": None,
    }
    # Newer h2load: "request : min max median p95 p99 mean sd +/-sd"
    m = re.search(r"^request\s*:\s*(.+)$", text, re.M)
    hdr = re.search(r"^\s+min\s+max\s+(.+)$", text, re.M)
    if m:
        cols = m.group(1).split()
        names = ["min", "max"] + (hdr.group(1).split() if hdr else ["mean", "sd", "+/-"])
        vals = dict(zip(names, cols))
        out["max_us"] = to_us(vals.get("max", ""))
        out["mean_us"] = to_us(vals.get("mean", ""))
        out["p50_us"] = to_us(vals.get("median", "")) if "median" in vals else None
        out["p99_us"] = to_us(vals.get("p99", "")) if "p99" in vals else None
    return out


def peak_rss_mb(path):
    try:
        vals = [int(x) for x in path.read_text().split() if x.isdigit()]
    except FileNotFoundError:
        return None
    return max(vals) / 1024 if vals else None


def load(dirpath):
    env = {}
    for line in (dirpath / "env.txt").read_text().splitlines():
        k, _, v = line.partition("=")
        env[k] = v
    rows = []
    with open(dirpath / "manifest.tsv") as f:
        for r in csv.DictReader(f, delimiter="\t"):
            raw = (dirpath / "raw" / f"{r['name']}.txt").read_text(errors="replace")
            parsed = parse_wrk(raw) if r["tool"] == "wrk" else parse_h2load(raw)
            if parsed is None:
                print(f"warning: could not parse raw/{r['name']}.txt", file=sys.stderr)
                continue
            r.update(parsed)
            for k in ("conns", "threads", "streams"):
                r[k] = int(r[k])
            r["peak_rss_mb"] = peak_rss_mb(dirpath / "raw" / f"{r['name']}.rss")
            rows.append(r)
    status = None
    if (dirpath / "status.json").exists():
        try:
            status = json.loads((dirpath / "status.json").read_text())
        except ValueError:
            pass
    skipped = (dirpath / "skipped.txt").read_text().splitlines() if (dirpath / "skipped.txt").exists() else []
    return env, rows, status, skipped


# ----------------------------------------------------------------- formatting

def fnum(x):
    return "—" if x is None else f"{x:,.0f}"


def flat(us):
    if us is None:
        return "—"
    if us >= 1e6:
        return f"{us / 1e6:.2f} s"
    if us >= 1e3:
        return f"{us / 1e3:.2f} ms"
    return f"{us:.0f} µs"


def fmb(x):
    return "—" if x is None else f"{x:,.1f}"


def short_rps(x):
    if x >= 1e6:
        return f"{x / 1e6:.2f}M"
    if x >= 1e3:
        return f"{x / 1e3:.0f}k"
    return f"{x:.0f}"


LABELS = {
    "origin": "Origin server, direct (no proxy)",
    "h1-hit": "HTTP/1.1, cache hit",
    "h1-miss": "HTTP/1.1, proxied to origin (cache off)",
    "h1-tls-hit": "HTTPS (HTTP/1.1 + TLS 1.3), cache hit",
    "h2-hit": "HTTP/2 over TLS, cache hit",
    "h3-hit": "HTTP/3 over QUIC, cache hit",
    "h1-newconn": "HTTP/1.1, new TCP connection per request",
    "tls-handshake": "HTTPS, full TLS handshake per request",
}


def label(r):
    return LABELS.get(r["name"], r["name"])


# ----------------------------------------------------------------- SVG charts

PALETTE = {"bar": "#4C78A8", "bar2": "#9ECAE9", "line": "#E45756", "ink": "#222", "grid": "#ddd", "muted": "#666"}
FONT = 'font-family="-apple-system,Segoe UI,Helvetica,Arial,sans-serif"'


def svg_wrap(w, h, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" {FONT}>'
            f'<rect width="{w}" height="{h}" fill="#fff"/>'
            f'<text x="16" y="26" font-size="16" font-weight="600" fill="{PALETTE["ink"]}">{title}</text>'
            f"{body}</svg>\n")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def nice_max(v):
    if v <= 0:
        return 1
    import math
    e = 10 ** math.floor(math.log10(v))
    for m in (1, 2, 2.5, 5, 10):
        if v <= m * e:
            return m * e
    return 10 * e


def hbar_chart(items, title, unit="req/s"):
    """items: [(label, value)] -> horizontal bar chart."""
    w, row, left, top = 760, 34, 330, 48
    h = top + row * len(items) + 30
    vmax = nice_max(max(v for _, v in items))
    plot = w - left - 90
    body = []
    for i in range(5):
        x = left + plot * i / 4
        body.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{h - 28}" stroke="{PALETTE["grid"]}"/>')
        body.append(f'<text x="{x:.1f}" y="{h - 12}" font-size="11" fill="{PALETTE["muted"]}" '
                    f'text-anchor="middle">{short_rps(vmax * i / 4)}</text>')
    for i, (lab, v) in enumerate(items):
        y = top + i * row
        bw = plot * v / vmax
        body.append(f'<text x="{left - 10}" y="{y + 17}" font-size="12.5" fill="{PALETTE["ink"]}" '
                    f'text-anchor="end">{esc(lab)}</text>')
        body.append(f'<rect x="{left}" y="{y + 4}" width="{bw:.1f}" height="{row - 12}" rx="3" fill="{PALETTE["bar"]}"/>')
        body.append(f'<text x="{left + bw + 6:.1f}" y="{y + 17}" font-size="12" font-weight="600" '
                    f'fill="{PALETTE["ink"]}">{short_rps(v)}</text>')
    body.append(f'<text x="{w - 16}" y="26" font-size="11" fill="{PALETTE["muted"]}" text-anchor="end">{unit}</text>')
    return svg_wrap(w, h, "".join(body), esc(title))


def combo_chart(xs, bars, line, title, xlabel, bar_label, line_label, line_fmt, ideal=None):
    """Vertical bars (left axis) with an optional line (right axis); categorical x."""
    w, h, left, right, top, bottom = 760, 380, 70, 80, 72, 60
    pw, ph = w - left - right, h - top - bottom
    n = len(xs)
    slot = pw / n
    bmax = nice_max(max(bars + (ideal or [])))
    body = []
    for i in range(5):
        y = top + ph - ph * i / 4
        body.append(f'<line x1="{left}" y1="{y:.1f}" x2="{left + pw}" y2="{y:.1f}" stroke="{PALETTE["grid"]}"/>')
        body.append(f'<text x="{left - 8}" y="{y + 4:.1f}" font-size="11" fill="{PALETTE["muted"]}" '
                    f'text-anchor="end">{short_rps(bmax * i / 4)}</text>')
    for i, (x, v) in enumerate(zip(xs, bars)):
        cx = left + slot * (i + 0.5)
        bh = ph * v / bmax
        bw = slot * 0.55
        body.append(f'<rect x="{cx - bw / 2:.1f}" y="{top + ph - bh:.1f}" width="{bw:.1f}" height="{bh:.1f}" '
                    f'rx="3" fill="{PALETTE["bar"]}"/>')
        body.append(f'<text x="{cx:.1f}" y="{top + ph - bh - 6:.1f}" font-size="11" font-weight="600" '
                    f'text-anchor="middle" fill="{PALETTE["ink"]}">{short_rps(v)}</text>')
        body.append(f'<text x="{cx:.1f}" y="{top + ph + 18}" font-size="12" text-anchor="middle" '
                    f'fill="{PALETTE["ink"]}">{esc(str(x))}</text>')
    if ideal:
        pts = " ".join(f"{left + slot * (i + 0.5):.1f},{top + ph - ph * v / bmax:.1f}" for i, v in enumerate(ideal))
        body.append(f'<polyline points="{pts}" fill="none" stroke="{PALETTE["muted"]}" stroke-dasharray="5,4"/>')
    if line and any(v is not None for v in line):
        lmax = nice_max(max(v for v in line if v is not None))
        pts = []
        for i, v in enumerate(line):
            if v is None:
                continue
            cx, cy = left + slot * (i + 0.5), top + ph - ph * v / lmax
            pts.append(f"{cx:.1f},{cy:.1f}")
            body.append(f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="3.5" fill="{PALETTE["line"]}"/>')
        body.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="{PALETTE["line"]}" stroke-width="2"/>')
        for i in range(5):
            y = top + ph - ph * i / 4
            body.append(f'<text x="{left + pw + 8}" y="{y + 4:.1f}" font-size="11" fill="{PALETTE["line"]}">'
                        f'{line_fmt(lmax * i / 4)}</text>')
    body.append(f'<text x="{left + pw / 2}" y="{h - 14}" font-size="12" text-anchor="middle" '
                f'fill="{PALETTE["muted"]}">{esc(xlabel)}</text>')
    lx = left
    body.append(f'<rect x="{lx}" y="{top - 20}" width="12" height="12" fill="{PALETTE["bar"]}"/>'
                f'<text x="{lx + 16}" y="{top - 10}" font-size="11.5" fill="{PALETTE["ink"]}">{esc(bar_label)}</text>')
    if line:
        body.append(f'<line x1="{lx + 170}" y1="{top - 14}" x2="{lx + 190}" y2="{top - 14}" stroke="{PALETTE["line"]}" '
                    f'stroke-width="2"/><text x="{lx + 195}" y="{top - 10}" font-size="11.5" '
                    f'fill="{PALETTE["ink"]}">{esc(line_label)}</text>')
    if ideal:
        body.append(f'<line x1="{lx + 340}" y1="{top - 14}" x2="{lx + 360}" y2="{top - 14}" stroke="{PALETTE["muted"]}" '
                    f'stroke-dasharray="5,4"/><text x="{lx + 365}" y="{top - 10}" font-size="11.5" '
                    f'fill="{PALETTE["ink"]}">linear scaling</text>')
    return svg_wrap(w, h, "".join(body), esc(title))


# ----------------------------------------------------------------- report

def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = Path(sys.argv[1])
    env, rows, status, skipped = load(d)
    by = {r["name"]: r for r in rows}
    group = lambda g: [r for r in rows if r["group"] == g]
    charts = d / "charts"
    charts.mkdir(exist_ok=True)

    (d / "results.json").write_text(json.dumps({"env": env, "results": rows, "status": status}, indent=2))

    md = []
    w = md.append
    w("# aproxy benchmark results\n")
    w(f"Generated by `bench/run.sh` on {env.get('date', '?')} at commit `{env.get('commit', '?')}`. "
      "Re-run with `./bench/run.sh`; raw tool output for every number is kept in "
      "`bench/results/<timestamp>/raw/`.\n")

    # ---- highlights
    hl = []
    main_rows = [r for r in rows if r["group"] in ("main", "sweep-conns", "sweep-workers", "payload")]
    if main_rows:
        best = max(main_rows, key=lambda r: r["rps"])
        desc = label(best) if best["group"] == "main" else (
            "HTTP/2 over TLS, cache hit" if best["tool"] == "h2load" else "HTTP/1.1, cache hit")
        hl.append(f"**{best['rps']:,.0f} requests/sec** peak throughput ({desc}, "
                  f"{best['conns']:,} connections, {best['workers']} worker threads)")
    if "h1-hit" in by:
        r = by["h1-hit"]
        hl.append(f"**{r['rps']:,.0f} req/s** on cache hits over HTTP/1.1 with **p99 latency of {flat(r['p99_us'])}** "
                  f"at {r['conns']} concurrent connections")
    if "h1-miss" in by:
        r = by["h1-miss"]
        s = f"**{r['rps']:,.0f} req/s** reverse-proxied to load-balanced upstreams (p99 {flat(r['p99_us'])})"
        if "origin" in by and by["origin"]["rps"] and "h1-hit" in by:
            s += f"; the cache serves **{by['h1-hit']['rps'] / r['rps']:.1f}×** more requests than the proxied path"
        hl.append(s)
    if "h1-tls-hit" in by and "h1-hit" in by and by["h1-hit"]["rps"]:
        r = by["h1-tls-hit"]
        hl.append(f"**{r['rps']:,.0f} req/s over TLS 1.3**, {100 * r['rps'] / by['h1-hit']['rps']:.0f}% of plaintext "
                  f"throughput (p99 {flat(r['p99_us'])})")
    if "h2-hit" in by:
        r = by["h2-hit"]
        hl.append(f"**{r['rps']:,.0f} req/s over HTTP/2** with {r['conns']} connections × {r['streams']} multiplexed streams")
    if "h3-hit" in by:
        r = by["h3-hit"]
        hl.append(f"**{r['rps']:,.0f} req/s over HTTP/3 (QUIC)** with {r['conns']} connections × {r['streams']} streams")
    if "tls-handshake" in by:
        hl.append(f"**{by['tls-handshake']['rps']:,.0f} full TLS handshakes/sec**")
    sc = group("sweep-conns")
    if sc:
        clean = [r for r in sc if r["socket_errors"] == 0 and r["errors"] == 0] or sc
        top = max(clean, key=lambda r: r["conns"])
        hl.append(f"**{top['conns']:,} concurrent keep-alive connections** handled with zero errors at "
                  f"{top['rps']:,.0f} req/s (p99 {flat(top['p99_us'])})"
                  + (f", using {top['peak_rss_mb']:.0f} MB RSS" if top["peak_rss_mb"] else ""))
    sw = sorted(group("sweep-workers"), key=lambda r: int(r["param"]))
    if len(sw) >= 2 and sw[0]["rps"]:
        a, b = sw[0], sw[-1]
        proto = "HTTP/2" if a["tool"] == "h2load" else "HTTP/1.1"
        hl.append(f"**{b['rps'] / a['rps']:.1f}× throughput scaling** from {a['param']} to {b['param']} worker threads "
                  f"({a['rps']:,.0f} → {b['rps']:,.0f} req/s over {proto}), one shared-nothing event loop per thread")
    pl = group("payload")
    if pl:
        big = max(pl, key=lambda r: int(r["param"]))
        hl.append(f"**{big['mbps'] * 8 / 1000:.1f} Gbit/s** served from cache with {int(big['param']) // 1024} KiB responses")
    if hl:
        w("## Highlights\n")
        for h in hl:
            w(f"- {h}")
        w("")
        w("<details><summary>Resume-ready one-liner</summary>\n")
        parts = []
        if main_rows:
            parts.append(f"{short_rps(best['rps'])} req/s")
        if "h1-hit" in by:
            parts.append(f"p99 {flat(by['h1-hit']['p99_us'])}")
        if sc:
            parts.append(f"{top['conns']:,} concurrent connections")
        if len(sw) >= 2:
            parts.append(f"{b['rps'] / a['rps']:.1f}× multi-core scaling")
        w("> Built a multi-threaded, event-driven (kqueue/epoll) reverse proxy in C with HTTP/1.1, HTTP/2, HTTP/3 (QUIC), "
          f"TLS 1.3 termination, load balancing and a sharded LRU cache; benchmarked at {', '.join(parts)} "
          f"on a {env.get('cores', '?')}-core {env.get('cpu', 'machine')}.\n")
        w("</details>\n")

    # ---- environment
    w("## Test environment\n")
    w("| | |\n|---|---|")
    for k, lab in [("cpu", "CPU"), ("cores", "Logical cores"), ("mem_gb", "Memory (GB)"), ("os", "OS"),
                   ("proxy", "Proxy build"), ("compiler", "Compiler"), ("wrk", "wrk"), ("h2load", "h2load"),
                   ("proxy_workers", "Proxy worker threads"), ("wrk_threads", "Load-generator threads"),
                   ("duration_s", "Measured duration per run (s)"), ("warmup_s", "Warm-up per run (s)"),
                   ("pinned", "CPU pinning (taskset)")]:
        if k in env:
            w(f"| {lab} | {env[k]} |")
    w("\nLoad generator, proxy and origin servers share the same machine over loopback, so absolute numbers "
      "are a lower bound on what the proxy alone can do. Compare runs only on the same hardware.\n")

    def table(rs, first_col, first_val):
        w(f"| {first_col} | Conns | Req/s | p50 | p90 | p99 | p99.9 | Max | Errors | MB/s | Peak RSS (MB) |")
        w("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for r in rs:
            conns = f"{r['conns']}×{r['streams']}" if r["streams"] > 1 else f"{r['conns']}"
            w(f"| {first_val(r)} | {conns} | **{fnum(r['rps'])}** | {flat(r['p50_us'])} | {flat(r['p90_us'])} | "
              f"{flat(r['p99_us'])} | {flat(r['p999_us'])} | {flat(r['max_us'])} | {r['errors'] + r['socket_errors']:,} | "
              f"{fmb(r['mbps'])} | {fmb(r['peak_rss_mb'])} |")
        w("")

    core = [r for r in rows if r["group"] in ("baseline", "main", "conn")]
    if core:
        w("## Scenarios\n")
        w("![Throughput by scenario](charts/throughput.svg)\n")
        table(core, "Scenario", label)
        (charts / "throughput.svg").write_text(
            hbar_chart([(label(r), r["rps"]) for r in core], "Throughput by scenario"))
        if any(r["tool"] == "h2load" for r in core):
            w("HTTP/2 and HTTP/3 rows come from h2load, which reports median, p99, mean and max only. "
              "`Conns` shows connections × concurrent streams.\n")

    if sc:
        w("## Concurrency sweep (HTTP/1.1 cache hit)\n")
        w("![Concurrency sweep](charts/concurrency.svg)\n")
        table(sc, "Connections", lambda r: f"{r['conns']:,}")
        (charts / "concurrency.svg").write_text(combo_chart(
            [f"{r['conns']:,}" for r in sc], [r["rps"] for r in sc], [r["p99_us"] / 1e3 for r in sc],
            "Throughput and p99 latency vs. concurrent connections", "concurrent connections",
            "requests/sec", "p99 latency (ms)", lambda v: f"{v:.1f} ms"))

    if sw:
        proto = "HTTP/2 over TLS" if sw[0]["tool"] == "h2load" else "HTTP/1.1"
        w(f"## Multi-core scaling ({proto}, cache hit)\n")
        w("![Worker scaling](charts/scaling.svg)\n")
        if sw[0]["tool"] == "h2load":
            w(f"Measured with h2load ({sw[0]['conns']} connections × {sw[0]['streams']} streams). Over loopback, "
              "HTTP/1.1 with one request per round trip saturates the load generator before a single proxy worker "
              "is busy, so it cannot show scaling. On CPUs with performance and efficiency cores, the extra "
              "workers land on slower cores and gains flatten.\n")
        base = sw[0]["rps"] / int(sw[0]["param"]) if sw[0]["rps"] else 0
        w("| Worker threads | Req/s | Speed-up | Efficiency | p99 / max |")
        w("|---:|---:|---:|---:|---:|")
        for r in sw:
            n = int(r["param"])
            sp = r["rps"] / sw[0]["rps"] if sw[0]["rps"] else 0
            eff = r["rps"] / (base * n) if base else 0
            w(f"| {n} | **{fnum(r['rps'])}** | {sp:.2f}× | {100 * eff:.0f}% | {flat(r['p99_us'])} / {flat(r['max_us'])} |")
        w("")
        (charts / "scaling.svg").write_text(combo_chart(
            [r["param"] for r in sw], [r["rps"] for r in sw], None,
            "Throughput vs. worker threads", "worker threads (one event loop each)",
            "requests/sec", "", None, ideal=[base * int(r["param"]) for r in sw]))

    if pl:
        w("## Response size (HTTP/1.1 cache hit)\n")
        w("| Body size | Req/s | Throughput | p50 | p99 |")
        w("|---:|---:|---:|---:|---:|")
        for r in sorted(pl, key=lambda r: int(r["param"])):
            n = int(r["param"])
            w(f"| {n // 1024} KiB | **{fnum(r['rps'])}** | {r['mbps'] * 8 / 1000:.2f} Gbit/s | "
              f"{flat(r['p50_us'])} | {flat(r['p99_us'])} |")
        w("")

    if status:
        c = status.get("cache", {})
        hits, misses = c.get("hits", 0), c.get("misses", 0)
        w("## Proxy counters at the end of the run\n")
        w(f"From `GET /__status`: {status.get('requests', 0):,} requests on the final proxy instance, "
          f"{status.get('connections', {}).get('total', 0):,} connections, cache hit ratio "
          f"{100 * hits / (hits + misses) if hits + misses else 0:.2f}% ({hits:,} hits / {misses:,} misses), "
          f"{status.get('upstream', {}).get('errors', 0):,} upstream errors.\n")

    if skipped:
        w("## Skipped\n")
        for s in skipped:
            w(f"- {s}")
        w("")

    w("## Methodology\n")
    w("- Each scenario gets a warm-up run that is thrown away, then one measured run of fixed duration.")
    w("- Requests/sec counts **successful (2xx/3xx) responses only**; any socket error, timeout or "
      "4xx/5xx is listed under Errors.")
    w("- Latency percentiles are measured by wrk (HTTP/1.1) and h2load (HTTP/2, HTTP/3). wrk is a "
      "closed-loop generator, so the percentiles are subject to coordinated omission; treat p99.9 / max "
      "as indicative.")
    w("- Origins are `bench/origin.c` (two instances, round-robin), a minimal C server, so that the "
      "proxied path measures the proxy rather than the backend.")
    w("- The access log is disabled (`log_level warn`); the config is `bench/lib/proxy.conf.in`.")
    w("- `Peak RSS` is the proxy's resident memory sampled every 0.5 s during the run.")
    if env.get("somaxconn", "").isdigit() and int(env["somaxconn"]) < 1024:
        w(f"- The OS listen backlog limit (somaxconn) was {env['somaxconn']}, so connect errors at high "
          "connection counts or in connection-per-request runs can come from the kernel dropping SYNs "
          "during bursts rather than from the proxy.")
    (d / "RESULTS.md").write_text("\n".join(md) + "\n")
    print(f"wrote {d / 'RESULTS.md'}, {d / 'results.json'} and {charts}/")


if __name__ == "__main__":
    main()
