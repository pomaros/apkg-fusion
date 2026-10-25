# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""HTTP fixtures whose readiness does not depend on reverse DNS."""
import http.server
import socketserver


class NumericIdentity:
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = self.server_address[0]
        self.server_port = self.server_address[1]


class HTTPServer(NumericIdentity, http.server.HTTPServer):
    pass


class ThreadingHTTPServer(NumericIdentity, http.server.ThreadingHTTPServer):
    pass
