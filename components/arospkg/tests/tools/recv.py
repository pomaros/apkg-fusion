#!/usr/bin/env python3
"""Receive files from the AROS guest over its network: HTTP PUT /<name>.

The guest's shared FAT directory corrupts guest writes, and its serial port
delivers nothing, so the network is the only reliable guest-to-host path.
QEMU's user networking maps the guest's 10.0.2.2 to this host's loopback.

usage: recv.py OUTDIR [PORT]   (listens on 127.0.0.1 only)
Each file is written to OUTDIR/<name> as it arrives (streamed, not buffered in
memory); the response body and the log line carry its size and sha256, so the
sender can compare with the hash it computed itself.
"""
import hashlib, http.server, os, sys

OUT = os.path.abspath(sys.argv[1]); PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
os.makedirs(OUT, exist_ok=True)

class H(http.server.BaseHTTPRequestHandler):
    def do_PUT(self):
        name = os.path.basename(self.path.lstrip('/')) or 'upload.bin'
        left = int(self.headers.get('Content-Length', 0))
        h = hashlib.sha256(); n = 0
        with open(os.path.join(OUT, name + '.part'), 'wb') as f:
            while left:
                chunk = self.rfile.read(min(left, 1 << 20))
                if not chunk:
                    break
                f.write(chunk); h.update(chunk); n += len(chunk); left -= len(chunk)
        complete = left == 0
        if complete:
            os.replace(os.path.join(OUT, name + '.part'), os.path.join(OUT, name))
        msg = f"{'OK' if complete else 'INCOMPLETE'} {name} {n} {h.hexdigest()}\n"
        sys.stdout.write(msg); sys.stdout.flush()
        self.send_response(200 if complete else 400)
        self.send_header('Content-Length', str(len(msg))); self.end_headers()
        self.wfile.write(msg.encode())
    def log_message(self, *a):
        pass

http.server.ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
