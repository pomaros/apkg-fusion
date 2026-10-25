#!/usr/bin/env python3
"""Before a push: every report directory that has a MANIFEST.sha256 must hold
exactly the files it lists (plus README.md and the manifest), with those
hashes. Catches files from other runs or sessions copied off a shared
results disk. Exits 1 on any difference.

    tools/check-reports.py [docs/reports]
"""
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "docs/reports")
bad = 0
for man in sorted(root.glob("*/MANIFEST.sha256")):
    d = man.parent
    listed = {}
    for line in man.read_text().splitlines():
        sha, name = line.split(None, 1)
        listed[name.strip()] = sha
    present = {p.name for p in d.iterdir() if p.is_file()} - {"README.md", "MANIFEST.sha256"}
    for extra in sorted(present - set(listed)):
        print(f"{d.name}: {extra} is not in the manifest"); bad += 1
    for name, sha in listed.items():
        p = d / name
        if not p.exists():
            print(f"{d.name}: {name} is missing"); bad += 1
        elif hashlib.sha256(p.read_bytes()).hexdigest() != sha:
            print(f"{d.name}: {name} changed since it was taken"); bad += 1
print(f"{len(list(root.glob('*/MANIFEST.sha256')))} manifest(s) checked, {bad} problem(s)")
sys.exit(1 if bad else 0)
