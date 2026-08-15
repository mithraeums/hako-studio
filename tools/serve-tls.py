#!/usr/bin/env python3
"""Serve dist/ over HTTPS with a self-signed certificate.

Not for production — a real host has a real certificate. This exists because the
browser build needs a *secure context*: OPFS, service workers and module workers
are unavailable over plain HTTP to anything but localhost. Testing the wasm from
a phone or tablet on the LAN therefore needs TLS, even on a private network.

    python3 tools/serve-tls.py [dist] [port]

The certificate is generated once into ~/.hako/certs and covers every local
address this machine answers on, so one accepted warning covers every device.
"""

import http.server
import ipaddress
import os
import socket
import ssl
import subprocess
import sys

CERT_DIR = os.path.expanduser("~/.hako/certs")
CERT = os.path.join(CERT_DIR, "studio.crt")
KEY = os.path.join(CERT_DIR, "studio.key")


def local_addresses():
    """Every IPv4 address worth putting in the certificate."""
    addrs = {"127.0.0.1"}
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("inet ") and "127.0.0.1" not in line:
                addrs.add(line.split()[1])
    except Exception:
        pass
    return sorted(addrs)


def ensure_cert():
    if os.path.exists(CERT) and os.path.exists(KEY):
        return
    os.makedirs(CERT_DIR, mode=0o700, exist_ok=True)
    ips = local_addresses()
    san = ",".join(
        [f"IP:{a}" for a in ips] + ["DNS:localhost", f"DNS:{socket.gethostname()}"]
    )
    print(f"generating a self-signed certificate for {san}")
    subprocess.run(
        [
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", KEY, "-out", CERT, "-days", "825",
            "-subj", "/CN=hako studio (local)",
            "-addext", f"subjectAltName={san}",
            # iOS refuses a certificate without serverAuth EKU outright — no
            # "visit anyway" escape — and caps self-signed validity at 825 days.
            "-addext", "extendedKeyUsage=serverAuth",
            "-addext", "basicConstraints=critical,CA:FALSE",
            "-addext", "keyUsage=critical,digitalSignature,keyEncipherment",
        ],
        check=True,
        capture_output=True,
    )
    os.chmod(KEY, 0o600)


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "dist"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8443
    if not os.path.isdir(root):
        sys.exit(f"{root}/ not found — run `make site` first")

    ensure_cert()
    os.chdir(root)

    class Handler(http.server.SimpleHTTPRequestHandler):
        extensions_map = {
            **http.server.SimpleHTTPRequestHandler.extensions_map,
            ".wasm": "application/wasm",     # required for streaming instantiation
            ".js": "text/javascript",
            ".webmanifest": "application/manifest+json",
        }

        def end_headers(self):
            # The page and the wasm beside it move together; a cached pair that
            # disagrees is a confusing failure.
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def log_message(self, fmt, *args):
            sys.stderr.write("  %s %s\n" % (self.address_string(), fmt % args))

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)

    class Server(http.server.ThreadingHTTPServer):
        # A rejected certificate fails during the handshake, before any request
        # line exists — without this, a refusing device looks like silence.
        def handle_error(self, request, client_address):
            err = sys.exc_info()[1]
            sys.stderr.write(f"  {client_address[0]} handshake failed: {err}\n")

    httpd = Server(("0.0.0.0", port), Handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    print(f"serving {root}/ over https on port {port}")
    for a in local_addresses():
        print(f"  https://{a}:{port}")
    print("\nThe certificate is self-signed: the first visit on each device warns.")
    print("On iOS: Advanced -> Visit this Website. That is what makes OPFS available.")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
