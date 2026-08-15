#!/usr/bin/env python3
"""Serve dist/ with correct types and no caching.

A browser holding a stale worker or wasm while the page is fresh produces
errors that read as logic bugs — a null module, a missing handler — so nothing
here is cacheable.

    python3 tools/serve-static.py [dist] [port]
"""

import functools
import http.server
import sys


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "dist"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8081

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {
            **http.server.SimpleHTTPRequestHandler.extensions_map,
            ".wasm": "application/wasm",
            ".js": "text/javascript",
            ".webmanifest": "application/manifest+json",
        }

        def end_headers(self):
            self.send_header("Cache-Control", "no-store, must-revalidate")
            super().end_headers()

        def send_response(self, code, message=None):
            # A conditional request must not be answered "unchanged".
            super().send_response(200 if code == 304 else code, message)

        def log_message(self, fmt, *args):
            sys.stderr.write("  %s\n" % (fmt % args))

    http.server.ThreadingHTTPServer.protocol_version = "HTTP/1.1"
    httpd = http.server.ThreadingHTTPServer(
        ("127.0.0.1", port), functools.partial(Handler, directory=root)
    )
    print(f"serving {root}/ on 127.0.0.1:{port} (no-store)")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
