#!/usr/bin/env python3
"""Write a TEST index of candidates, for trying them on AROS before approval.

Not the published index and never uploaded. mkindex.py publishes approved
manifests only; this exists so that a candidate can be installed, started
and removed on the target before there is an approved manifest, and the
result recorded as a runtime test.

    candidate_index.py OUT id...
"""
import hashlib, json, pathlib, sys, zipfile

HERE = pathlib.Path(__file__).resolve().parent.parent
cands = json.load(open(HERE / "index/candidates.json"))["candidates"]
items = {c["id"]: c for c in (cands.values() if isinstance(cands, dict) else cands)}
reqs = json.load(open(sys.argv[2])) if sys.argv[2].endswith(".json") else {}
ids = sys.argv[3:] if reqs else sys.argv[2:]

out = []
for i in ids:
    key = next((k for k in items if k == i or k.startswith(i)), None)
    ch = items[key]["chosen"]
    z = next(p for p in (HERE / ".cache/archives").rglob(ch["filename"]))
    names = zipfile.ZipFile(z).namelist()
    tops = sorted({n.split("/")[0] for n in names if "/" in n})
    loose = [n for n in names if "/" not in n]
    drawer = tops[0] if len(tops) == 1 else ""
    icon = drawer + ".info" if drawer + ".info" in loose else ""
    out.append({
        "id": i, "version": ch.get("version") or "0", "summary": ch["summary"],
        "arch": "x86_64", "abi": ch["abi"], "category": ch["category"],
        "url": ch["url"], "size": z.stat().st_size,
        "sha256": hashlib.sha256(z.read_bytes()).hexdigest(),
        "depends": [], "kind": "app", "subdir": drawer, "icon": icon, "install": {},
        "requires_system": [{"type": "library", "id": r} for r in reqs.get(i, [])],
    })
json.dump({"schema": 1, "source": "TEST candidates -- not a repository", "packages": out},
          open(sys.argv[1], "w"), indent=1)
print(f"{len(out)} candidates -> {sys.argv[1]}")
for p in out:
    print(f"  {p['id']:16} subdir={p['subdir']:18} icon={p['icon'] or '-':22} req={len(p['requires_system'])}")
