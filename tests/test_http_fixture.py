# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Loopback responses work even when reverse DNS is unavailable."""
import http.client
import http.server
import socket
import threading
from unittest.mock import patch
from http_fixture import HTTPServer, ThreadingHTTPServer


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"ok")


for server_type in (HTTPServer, ThreadingHTTPServer):
    with patch.object(socket, "getfqdn", side_effect=AssertionError("unexpected DNS lookup")):
        with server_type(("127.0.0.1", 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                client = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=5)
                client.request("GET", "/")
                response = client.getresponse()
                assert response.status == 200 and response.read() == b"ok"
                client.close()
            finally:
                server.shutdown()
                thread.join(timeout=5)
                assert not thread.is_alive()
print("HTTP fixtures: synchronous and threaded responses pass without reverse DNS")
