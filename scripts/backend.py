#!/usr/bin/env python3
"""Tiny HTTP/1.1 origin server used to demo and test the proxy.

    python3 scripts/backend.py 9001 [name]

Endpoints:
    /            -> text page naming this backend (cacheable, max-age=30)
    /health      -> 200 "ok"  (used by active health checks)
    /headers     -> JSON dump of the request headers it received
    /echo        -> echoes the request body back (POST)
    /nocache     -> Cache-Control: no-store
    /slow        -> sleeps 2 seconds before answering
    /status/NNN  -> responds with status NNN
    /big         -> 1 MiB body
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
NAME = sys.argv[2] if len(sys.argv) > 2 else f"backend-{PORT}"
COUNTER = {"n": 0}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"  # keep-alive, so the proxy pool is exercised
    server_version = "demo-backend/1.0"

    def log_message(self, fmt, *args):  # quieter output
        sys.stderr.write(f"[{NAME}] {self.command} {self.path}\n")

    def _send(self, status, body, ctype="text/plain", extra=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Backend", NAME)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def do_GET(self):
        COUNTER["n"] += 1
        path = self.path.split("?")[0]
        if path == "/health":
            self._send(200, "ok", extra={"Cache-Control": "no-store"})
        elif path == "/headers":
            self._send(200, json.dumps(dict(self.headers), indent=1), "application/json",
                       {"Cache-Control": "no-store"})
        elif path == "/nocache":
            self._send(200, f"{NAME} request #{COUNTER['n']}\n", extra={"Cache-Control": "no-store"})
        elif path == "/slow":
            time.sleep(2)
            self._send(200, f"slow response from {NAME}\n")
        elif path.startswith("/status/"):
            code = int(path.rsplit("/", 1)[1])
            self._send(code, f"status {code} from {NAME}\n", extra={"Cache-Control": "no-store"})
        elif path == "/big":
            self._send(200, b"x" * (1024 * 1024), "application/octet-stream")
        else:
            self._send(200, f"hello from {NAME} (request #{COUNTER['n']})\n",
                       extra={"Cache-Control": "public, max-age=30"})

    do_HEAD = do_GET

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        self._send(200, body, self.headers.get("Content-Type", "application/octet-stream"),
                   {"Cache-Control": "no-store"})


if __name__ == "__main__":
    print(f"{NAME} listening on :{PORT}", file=sys.stderr)
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
