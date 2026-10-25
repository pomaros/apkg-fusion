#!/usr/bin/env python3
"""When each application and each build first appeared in the catalogue.

    catalogue_history.py bootstrap INDEX_DIR [--out FILE]

history.json in arospkg-index keeps, for every variant (id, CPU, ABI) and
for every application id, the date it was first published, and the commit
that showed it. It is the source of `first_added` and `app_first_added` in
the catalogue, and of `apkg recent`.

The dates are publication dates of the catalogue, never a file's mtime nor
the day a client downloaded it, and they never move: a new version, a new
revision, a new review or a removal and return keep the first date.

`bootstrap` derives them from the git history of the published files
(index.json from the repository's first commit, index-v2.json from when it
appeared); the first commit is the beginning of the catalogue, so its
entries carry its date as a fact, not an estimate. After that, mkindex.py
adds each new variant on the day the publish workflow first writes it
(update()).

Dates are UTC days (YYYY-MM-DD) of the commit that published the entry.
Early entries had no "abi"; such an entry is credited to the build its
(id, CPU) got when the ABI was first written down.
"""
import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

PUBLISHED = ("index.json", "index-v2.json")


def vkey(pid, arch, abi):
    return f"{pid.lower()}/{arch}/{abi}"


def _git(repo, *args):
    return subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True, check=True).stdout


def bootstrap(repo):
    repo = Path(repo)
    log = _git(repo, "log", "--reverse", "--format=%H %ct", "--", *PUBLISHED).split("\n")
    variants, apps, pending = {}, {}, {}      # pending: (id, arch) without abi -> (date, commit)
    for line in log:
        if not line.strip():
            continue
        commit, ts = line.split()
        day = datetime.fromtimestamp(int(ts), timezone.utc).strftime("%Y-%m-%d")
        for name in PUBLISHED:
            r = subprocess.run(["git", "show", f"{commit}:{name}"], cwd=repo, capture_output=True, text=True)
            if r.returncode:
                continue
            try:
                pkgs = json.loads(r.stdout).get("packages", [])
            except ValueError:
                continue
            for p in pkgs:
                pid, arch, abi = p.get("id"), p.get("arch"), p.get("abi")
                if not pid or not arch:
                    continue
                apps.setdefault(pid.lower(), {"first_added": day, "commit": commit[:12]})
                if not abi:
                    pending.setdefault((pid.lower(), arch), (day, commit[:12]))
                    continue
                k = vkey(pid, arch, abi)
                if k not in variants:
                    early = pending.pop((pid.lower(), arch), None)
                    d, c = early if early else (day, commit[:12])
                    variants[k] = {"first_added": d, "commit": c}
    left = sorted(f"{i}/{a}" for i, a in pending)
    if left:
        print(f"note: published without an ABI and never with one: {', '.join(left)}", file=sys.stderr)
    return {"schema": 1, "variants": dict(sorted(variants.items())), "apps": dict(sorted(apps.items()))}


def load(path):
    """history.json, checked for shape. Raises ValueError with the reason."""
    h = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(h, dict) or h.get("schema") != 1:
        raise ValueError("not a schema 1 history")
    for part in ("variants", "apps"):
        if not isinstance(h.get(part), dict):
            raise ValueError(f"{part} is not an object")
        for k, v in h[part].items():
            d = v.get("first_added") if isinstance(v, dict) else None
            try:
                datetime.strptime(d or "", "%Y-%m-%d")
            except ValueError:
                raise ValueError(f"{part}.{k}: first_added {d!r} is not YYYY-MM-DD")
    return h


def update(h, packages, day, commit):
    """Add what is published for the first time. Returns the new keys."""
    added = []
    for p in packages:
        a = p["id"].lower()
        if a not in h["apps"]:
            h["apps"][a] = {"first_added": day, "commit": commit}
        k = vkey(p["id"], p["arch"], p["abi"])
        if k not in h["variants"]:
            h["variants"][k] = {"first_added": day, "commit": commit}
            added.append(k)
    h["variants"] = dict(sorted(h["variants"].items()))
    h["apps"] = dict(sorted(h["apps"].items()))
    return added


def dates_for(h, p):
    """(first_added, app_first_added) for a catalogue entry; None when unknown."""
    v = h["variants"].get(vkey(p["id"], p["arch"], p["abi"]))
    a = h["apps"].get(p["id"].lower())
    return (v or {}).get("first_added"), (a or {}).get("first_added")


def dump(h):
    return json.dumps(h, indent=1) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bootstrap")
    b.add_argument("index_dir")
    b.add_argument("--out")
    a = ap.parse_args()
    h = bootstrap(a.index_dir)
    text = dump(h)
    if a.out:
        Path(a.out).write_text(text, encoding="utf-8")
        print(f"{len(h['variants'])} variants, {len(h['apps'])} applications -> {a.out}")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
