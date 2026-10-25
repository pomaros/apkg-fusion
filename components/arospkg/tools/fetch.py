#!/usr/bin/env python3
"""Download an archive, verify its size, and record its SHA-256.

Separate from both import and index generation because it is the only step that
touches the network in bulk, and the only one that can leave a mess.

Cache correctness, which the first version got wrong in three ways:

  * The cache key is the full <category>/<filename> path, not the bare
    filename. The catalogue really does carry the same filename in different
    categories -- protrekkr.x86_64-aros-v11.zip exists under audio/edit and
    audio/tracker as different versions -- so keying on the name alone would
    serve one when asked for the other.
  * A download goes to <name>.part and is renamed into place only after it has
    completed AND its size matches the catalogue. An interrupted download can
    therefore never be picked up and hashed as if it were complete.
  * A size mismatch is a hard failure that blocks the entry. It used to print a
    warning and record the hash anyway, which is the same as not checking.
"""

import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import catalogue as cat


def fetch_one(category, filename, url, expected_size, cache_dir):
    """Return (path, sha256, size). Raises on any mismatch."""
    target = cache_dir / category / filename          # full path is the key
    target.parent.mkdir(parents=True, exist_ok=True)

    if not target.exists():
        part = target.with_suffix(target.suffix + ".part")
        if part.exists():
            part.unlink()                              # never resume; never trust
        got = 0
        with urllib.request.urlopen(url, timeout=180) as r, open(part, "wb") as f:
            while True:
                chunk = r.read(1 << 16)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
        if expected_size is not None and got != expected_size:
            part.unlink()
            raise ValueError(f"size mismatch: catalogue {expected_size}, downloaded {got}")
        part.rename(target)                            # publish only when whole

    size = target.stat().st_size
    if expected_size is not None and size != expected_size:
        raise ValueError(f"cached size {size} != catalogue {expected_size}")

    h = hashlib.sha256()
    with open(target, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return target, h.hexdigest(), size


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ids", nargs="+", help="candidate ids, or <id>.<arch>")
    ap.add_argument("--candidates", default="index/candidates.json")
    ap.add_argument("--cache", default=".cache/archives")
    args = ap.parse_args()

    data = json.loads(Path(args.candidates).read_text(encoding="utf-8"))
    wanted = set(args.ids)
    hits = [c for c in data["candidates"]
            if c["id"] in wanted or f"{c['id']}.{c['arch']}" in wanted]
    if not hits:
        sys.exit("no matching candidates")

    for c in hits:
        ch = c["chosen"]
        label = f"{c['id']}.{c['arch']}"
        try:
            path, sha, size = fetch_one(ch["category"], ch["filename"], ch["url"],
                                        ch["size"], Path(args.cache))
            print(f"OK   {label}")
            print(f"     {path}")
            print(f"     sha256 {sha}")
            print(f"     size   {size}")
        except Exception as exc:
            print(f"FAIL {label}: {exc}")


if __name__ == "__main__":
    main()
