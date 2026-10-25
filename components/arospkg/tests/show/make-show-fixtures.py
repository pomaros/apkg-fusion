#!/usr/bin/env python3
"""The index the `show` acceptance run reads: the ARexx run's packages
(soliton revision 1 or 2, cls, xrick) plus entries whose only purpose is to
be described -- one for another ABI, one for another CPU, one with an ABI
this client does not know, and rescode twice, for two targets.

    make-show-fixtures.py REPO OUTDIR      writes SHOWIDX1, SHOWIDX2 (and each reversed, ...R), BIGIDX,
                                           SOLR1, SOLR2, RESCODEZIP
"""
import json, os, shutil, subprocess, sys

repo, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
tmp = os.path.join(out, "_rx")
subprocess.run([sys.executable, os.path.join(repo, "tests/arexx/make-arexx-fixtures.py"), repo, tmp],
               check=True, stdout=subprocess.DEVNULL)
rel = json.load(open(os.path.join(repo, "tests/release-index.json")))["packages"]
rescode = dict([p for p in rel if p["id"] == "rescode"][0])
extra = [
    {"id": "oldthing", "version": "1.0", "summary": "built for mainline", "arch": "x86_64", "abi": "v1",
     "category": "test", "url": "https://example.invalid/oldthing.zip", "size": 1000, "sha256": "0" * 64,
     "kind": "app", "requires_system": [{"type": "library", "id": "crt.library"}]},
    {"id": "armthing", "version": "1.0", "summary": "built for another CPU", "arch": "aarch64", "abi": "v11",
     "category": "test", "url": "https://example.invalid/armthing.zip", "size": 1000, "sha256": "0" * 64,
     "kind": "app"},
    {"id": "futurething", "version": "1.0", "summary": "an ABI this client does not know", "arch": "i386",
     "abi": "v0", "category": "test", "url": "https://example.invalid/futurething.zip", "size": 1000,
     "sha256": "0" * 64, "kind": "app"},
    {"id": "rescode", "version": "1.2", "summary": "Calculates resistor values (32-bit build)", "arch": "i386",
     "abi": "v0", "category": "utility/misc", "url": "https://archives.arosworld.org/share/utility/misc/rescode.i386-aros.zip",
     "size": 29771, "sha256": "0" * 64, "kind": "app"},
    dict(rescode, source="https://example.invalid/rescode-source", license="Freeware"),
]
twin = {"id": "twin", "version": "1.0", "summary": "listed twice for the same target", "arch": "x86_64",
        "abi": "v11", "category": "test", "size": 1000, "sha256": "0" * 64, "kind": "app"}
extra += [dict(twin, url="https://example.invalid/twin-a.zip"), dict(twin, url="https://example.invalid/twin-b.zip")]
for n in ("1", "2"):
    base = json.load(open(os.path.join(tmp, "IDX" + n)))
    base["packages"] += extra
    if n == "2":
        # a newer revision of soliton for another target: an upgrade of the
        # installed x86_64 build must not cross to it
        sol = [p for p in base["packages"] if p["id"] == "soliton"][0]
        base["packages"].append(dict(sol, arch="i386", abi="v0", revision=3,
                                     url="https://example.invalid/soliton-i386.zip"))
    json.dump(base, open(os.path.join(out, "SHOWIDX" + n), "w"), indent=1)
    # the same entries in the opposite order: nothing may depend on it
    rev = dict(base, packages=list(reversed(base["packages"])))
    json.dump(rev, open(os.path.join(out, "SHOWIDX" + n + "R"), "w"), indent=1)
# ARexx LIST over this would answer more than 65535 bytes: 1100 ids of 63
# characters, few enough fields to stay under the parser's 8192 tokens
big = [{"id": ("p%04d-" % i) + "x" * 57, "arch": "x86_64", "abi": "v11"} for i in range(1100)]
json.dump({"schema": 1, "packages": big}, open(os.path.join(out, "BIGIDX"), "w"))
for f in ("SOLR1", "SOLR2"):
    shutil.copyfile(os.path.join(tmp, f), os.path.join(out, f))
# the native rescode, so installing it needs no network: which variant an
# install takes is what is being checked, not the download
src = os.path.join(repo, ".cache/archives", rescode["url"].split("/share/", 1)[1])
shutil.copyfile(src, os.path.join(out, "RESCODEZIP"))
import hashlib
assert hashlib.sha256(open(os.path.join(out, "RESCODEZIP"), "rb").read()).hexdigest() == rescode["sha256"]
shutil.rmtree(tmp)
print("fixtures in", out)
