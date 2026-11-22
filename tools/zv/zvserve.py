#!/usr/bin/env python3
"""zvserve.py -- serve a directory of .zv films to cinema over HTTP.

    tools/zv/zvserve.py [DIR] [--port 8000] [--bind ADDR]

cinema plays a film from a web server by asking for it a piece at a
time (docs/cinema_app.md, "From a web server"): each request is a byte
range, on a connection kept open between them. Any server that does
both will do -- nginx, Apache, Caddy. Python's own `http.server` does
neither (it sends the whole file and closes), which is what this is
for: the same thing with ranges and keep-alive, and nothing else.

It lists the films at / with the URL to type for each. Read-only, no
directory above DIR is reachable. For a home network; it is not a web
server to face the internet with.
"""

import argparse
import os
import socket
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, quote

CHUNK = 64 * 1024


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "zvserve/1"
    root = "."
    quiet = False

    def log_message(self, fmt, *args):
        if not self.quiet:
            sys.stderr.write("%s  %s\n" % (self.address_string(), fmt % args))

    def local_path(self):
        path = unquote(self.path.split("?", 1)[0].split("#", 1)[0])
        parts = [p for p in path.split("/") if p not in ("", ".")]
        if any(p == ".." for p in parts):
            return None
        return os.path.join(self.root, *parts)

    def plain(self, status, text, head=False):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head:
            self.wfile.write(body)

    def listing(self, head):
        host = self.headers.get("Host") or "%s:%d" % self.server.server_address[:2]
        lines = ["Films here, and what to type in cinema (u):", ""]
        for dirpath, _dirs, files in sorted(os.walk(self.root)):
            for name in sorted(files):
                if not name.lower().endswith(".zv"):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), self.root)
                size = os.path.getsize(os.path.join(dirpath, name))
                lines.append("  http://%s/%s   (%.1f MB)" % (host, quote(rel.replace(os.sep, "/")), size / 1e6))
        if len(lines) == 2:
            lines.append("  (no .zv files)")
        self.plain(200, "\n".join(lines) + "\n", head)

    def serve(self, head):
        p = self.local_path()
        if p is None:
            return self.plain(403, "no\n", head)
        if os.path.isdir(p):
            return self.listing(head)
        try:
            f = open(p, "rb")
        except OSError:
            return self.plain(404, "not found\n", head)
        with f:
            size = os.fstat(f.fileno()).st_size
            first, last = 0, size - 1
            status = 200
            rng = self.headers.get("Range")
            if rng and rng.startswith("bytes=") and "," not in rng:
                a, _, b = rng[6:].partition("-")
                try:
                    if a == "":                 # the last b bytes
                        first = max(0, size - int(b))
                    else:
                        first = int(a)
                        if b != "":
                            last = min(int(b), size - 1)
                    status = 206
                except ValueError:
                    status = 200
                    first, last = 0, size - 1
                if status == 206 and (first >= size or first > last):
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
            n = last - first + 1 if size else 0
            self.send_response(status)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(n))
            if status == 206:
                self.send_header("Content-Range", "bytes %d-%d/%d" % (first, last, size))
            self.end_headers()
            if head:
                return
            f.seek(first)
            while n > 0:
                d = f.read(min(CHUNK, n))
                if not d:
                    break
                self.wfile.write(d)
                n -= len(d)

    def do_GET(self):
        try:
            self.serve(False)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True

    def do_HEAD(self):
        self.serve(True)


def addresses():
    out = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        out.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    return out or ["127.0.0.1"]


def main():
    ap = argparse.ArgumentParser(description="serve .zv films to cinema")
    ap.add_argument("dir", nargs="?", default=".")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    Handler.root = os.path.abspath(a.dir)
    Handler.quiet = a.quiet
    Handler.timeout = 120           # an idle connection is let go
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    srv.daemon_threads = True
    for ip in addresses():
        print("zvserve: %s at http://%s:%d/" % (Handler.root, ip, a.port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
