#!/usr/bin/env python3
"""The index for the install-to-folder acceptance (tests/ux/), from the
published catalogue at a fixed commit, so every machine gets the same file.

    make-ux-index.py OUTFILE

It holds two real packages, both x86_64 ABIv11 from AROS Archives:
  gmore     with post_install_notes, added here only (the published entry
            has none, and the published index is not changed);
  zunecalc  without notes.
The notes follow the limits mkindex.py enforces, which this script checks.
"""
import hashlib, json, sys, urllib.request
from pathlib import Path

COMMIT = "28b7ca438a0a9c4653edeb210e9934d66f7f9145"
SHA256 = "24cf4fb2a0ee334644819c5cc5e069fb71fcafb765e98a28cb9d1bd55b0cca85"
URL = f"https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/{COMMIT}/index.json"
NOTES = [
    "Test note, line 1: this text comes from the tests/ux index, not from the author.",
    "Line 2: a second line, to show that lines stay apart.",
]

raw = urllib.request.urlopen(URL, timeout=60).read()
got = hashlib.sha256(raw).hexdigest()
if got != SHA256:
    sys.exit(f"index at {COMMIT} has SHA-256 {got}, expected {SHA256}")
idx = json.loads(raw)
pick = lambda i: next(p for p in idx["packages"] if p["id"] == i and p["arch"] == "x86_64" and p["abi"] == "v11")
gmore, zunecalc = dict(pick("gmore")), dict(pick("zunecalc"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mkindex  # noqa: E402
assert not mkindex.check_notes(NOTES), mkindex.check_notes(NOTES)
# The notes go right after the summary, ahead of url, size and sha256: an
# older client then has to read the fields after an unknown array correctly
# to install the package at all, not merely skip a key at the end.
gmore = {k: v for k, v in list(gmore.items())[:list(gmore).index("summary") + 1]} | \
        {"post_install_notes": NOTES} | \
        {k: v for k, v in list(gmore.items())[list(gmore).index("summary") + 1:]}
idx["packages"] = [gmore, zunecalc]
with open(sys.argv[1], "w") as f:
    json.dump(idx, f, indent=1)
    f.write("\n")
