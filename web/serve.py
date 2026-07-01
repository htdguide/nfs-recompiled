#!/usr/bin/env python3
"""Tiny static server that sends the COOP/COEP headers WebAssembly threads need.

SharedArrayBuffer (required by -pthread builds) is only exposed to cross-origin
isolated pages, so the browser must see:
    Cross-Origin-Opener-Policy:   same-origin
    Cross-Origin-Embedder-Policy: require-corp

Usage:  python3 web/serve.py [port] [directory]
        default port 8080, default directory = build-web
"""
import http.server
import socketserver
import sys
import os

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
DIRECTORY = sys.argv[2] if len(sys.argv) > 2 else "build-web"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)) + "/..")
    with socketserver.ThreadingTCPServer(("", PORT), Handler) as httpd:
        print(f"serving {DIRECTORY}/ at http://localhost:{PORT}/  (COOP/COEP on)")
        print(f"open   http://localhost:{PORT}/nfs2se.html")
        httpd.serve_forever()
