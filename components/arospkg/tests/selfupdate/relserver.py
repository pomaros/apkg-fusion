#!/usr/bin/env python3
"""A controlled arospkg release source for self-update tests on a pool guest.

    relserver.py CERTDIR RELEASEDIR [--fail NAME:HOW] ...

Serves RELEASEDIR over HTTPS on port 443 as https://10.0.2.2/<path>, which
is how a QEMU user-network guest reaches this host. apkg only speaks HTTPS
to port 443 and checks the certificate, so the server presents a
certificate for the IP 10.0.2.2 signed by a test CA (make-ca.sh); the
guest trusts that CA only for the test, by appending it to the RAM copy
of its bundle (ENV:SYS/Certificates/ca-bundle.crt), never ENVARC:.

Port 443 can only be had on all interfaces on macOS without root, so every
connection that does not come from 127.0.0.1 (where QEMU's slirp connects
from) is closed unanswered.

A release directory is laid out like a GitHub release: <tag>/SHA256SUMS
and <tag>/arospkg-<ver>.<target>.zip; the guest is pointed at
https://10.0.2.2/<tag>/ with --self-update-from (apkg) or --release-from
(PkgManager).

--fail NAME:HOW makes requests whose path ends in NAME fail, for tests of
the failure paths: HOW is a status code (404, 500), "cut:<bytes>" (send the
headers and that many bytes of the body, then close), or "slow:<ms>" (a
pause per 16 KB, so a test can cancel mid-way). Every request is logged
with its outcome to stdout.
"""
import http.server, os, socketserver, ssl, sys, time

CERTDIR, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
FAILS = {}
args = sys.argv[3:]
while args:
    if args[0] == "--fail" and len(args) > 1:
        name, how = args[1].split(":", 1)
        FAILS[name] = how
        args = args[2:]
    else:
        sys.exit("unknown argument " + args[0])


class H(http.server.BaseHTTPRequestHandler):
    def log(self, what):
        print(time.strftime("%H:%M:%S"), self.path, what, flush=True)

    def do_GET(self):
        rel = os.path.normpath(self.path.split("?", 1)[0].lstrip("/"))
        path = os.path.join(ROOT, rel)
        if rel.startswith("..") or not os.path.isfile(path):
            self.send_error(404); self.log("404 missing"); return
        how = next((h for n, h in FAILS.items() if self.path.endswith(n)), None)
        if how and how.isdigit():
            self.send_error(int(how)); self.log(how + " injected"); return
        data = open(path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if how and how.startswith("cut:"):
            n = int(how[4:])
            self.wfile.write(data[:n]); self.wfile.flush()
            self.close_connection = True
            self.log("cut after %d of %d bytes" % (n, len(data))); return
        step = 16384
        pause = int(how[5:]) / 1000.0 if how and how.startswith("slow:") else 0
        for i in range(0, len(data), step):
            self.wfile.write(data[i:i + step])
            if pause: time.sleep(pause)
        self.log("200 %d bytes" % len(data))

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def verify_request(self, request, client_address):
        return client_address[0] == "127.0.0.1"


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(os.path.join(CERTDIR, "server.crt"), os.path.join(CERTDIR, "server.key"))
srv = S(("0.0.0.0", 443), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("serving", ROOT, "as https://10.0.2.2/ (127.0.0.1 peers only)", flush=True)
srv.serve_forever()
