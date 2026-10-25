#!/usr/bin/env python3
"""The index for the TLS refusal, from the published catalogue at a fixed
commit, so the same file comes out on every machine.

    make-tls-index.py OUTFILE

It holds two entries:
  gmore   the published entry, unchanged: the operation after the refusal
          must succeed;
  tlsbad  a copy of gmore whose archive is on expired.badssl.com, a server
          kept up with an expired certificate. The client must refuse the
          connection; verification is not relaxed anywhere for this.
The published index is not changed; this file only goes into the test root.
"""
import copy, hashlib, json, sys, urllib.request

COMMIT = "fbe3d83a898654c42c798dbbe5f30bd380f2dc9a"
SHA256 = "39682d07dcdb75b5e0fb57fb68c1520205159cac9bb3a8882768dc6069e35105"
URL = f"https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/{COMMIT}/index.json"

raw = urllib.request.urlopen(URL, timeout=60).read()
got = hashlib.sha256(raw).hexdigest()
if got != SHA256:
    sys.exit(f"index at {COMMIT} has SHA-256 {got}, expected {SHA256}")
idx = json.loads(raw)
gmore = next(p for p in idx["packages"] if p["id"] == "gmore")
bad = copy.deepcopy(gmore)
bad["id"] = "tlsbad"
bad["summary"] = "test entry: archive on a server with an expired certificate"
bad["url"] = "https://expired.badssl.com/tlsbad.x86_64-aros-v11.zip"
idx["packages"] = [gmore, bad]
with open(sys.argv[1], "w") as f:
    json.dump(idx, f, indent=1)
    f.write("\n")
