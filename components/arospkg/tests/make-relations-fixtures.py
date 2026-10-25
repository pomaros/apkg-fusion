#!/usr/bin/env python3
"""The payload for relations on an AROS guest: small controlled packages for
every case, and the real Hardcode -> Bishop replacement.

    make-relations-fixtures.py OUTDIR --apkg BINARY [--pkgmanager BINARY]
        --hardcode ZIP --hardcode-index JSON --bishop ZIP --bishop-index JSON

OUTDIR gets apkg (and PkgManager), idx.json (one catalogue, in the shape
mkindex.py writes, with every case), cache/<id>.zip for each archive (the
names apkg caches them under, so nothing is downloaded except Limpet's
release, fetched here once), relations.script and expect.json.

  relapp -> rellib >= 1.2 -> relbase      transitive dependencies
  relold -> rellib >= 9                   the catalogue's version too old
  relabi -> relv1 (only x86_64/v1)        a dependency only for another ABI
  relamb -> reltwice (two builds)         ambiguous
  relcyc1 -> relcyc2 -> relcyc1           a cycle
  relghost -> relnone (absent)            not in the catalogue
  relenemy conflicts relbase              declared by the new package
  relfoe conflicts relpeace               declared by an installed package
  relbroken -> relbad (wrong sha256)      a plan that fails half-way
  hardcode -> limpet (real)               a real dependency
  bishop replaces hardcode, recommends limpet >= 0.4.1
"""
import argparse, hashlib, io, json, shutil, sys, urllib.request, zipfile
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--apkg", required=True)
ap.add_argument("--pkgmanager")
ap.add_argument("--hardcode", required=True)
ap.add_argument("--hardcode-index", required=True)
ap.add_argument("--bishop", required=True)
ap.add_argument("--bishop-index", required=True)
a = ap.parse_args()
out = Path(a.out)
if out.exists():
    shutil.rmtree(out)
(out / "cache").mkdir(parents=True)


def drawer_zip(name):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr(f"{name}/", b"")
        z.writestr(f"{name}/ReadMe", f"{name}: a test package for apkg relations\n".encode())
    return b.getvalue()


def entry(pid, version="1.0", abi="v11", data=None, **rel):
    data = data if data is not None else drawer_zip(pid.capitalize())
    (out / "cache" / f"{pid}.zip").write_bytes(data)
    e = {"id": pid, "version": version, "summary": f"{pid} (relations test)", "arch": "x86_64", "abi": abi,
         "category": "test", "url": f"https://example.invalid/rel/{pid}-{abi}.zip", "size": len(data),
         "sha256": hashlib.sha256(data).hexdigest(), "depends": rel.pop("depends", []),
         "requires_system": [], "kind": "app", "subdir": pid.capitalize(), "icon": "", "install": {}}
    e.update(rel)
    if any(e.get(k) for k in ("depends", "conflicts", "replaces")):
        e["client_features"] = ["relations"]
    return e


pk = [
    entry("relbase"), entry("rellib", "1.3", depends=["relbase"]), entry("relapp", depends=["rellib >= 1.2"]),
    entry("relold", depends=["rellib >= 9"]),
    entry("relv1", abi="v1"), entry("relabi", depends=["relv1"]),
    entry("reltwice"), entry("relamb", depends=["reltwice"]),
    entry("relcyc1", depends=["relcyc2"]), entry("relcyc2", depends=["relcyc1"]),
    entry("relghost", depends=["relnone"]),
    entry("relenemy", conflicts=["relbase"]),
    entry("relfoe", conflicts=["relpeace"]), entry("relpeace"),
    entry("relbroken", depends=["relbad"]),
]
twice2 = entry("reltwice", "2.0")
twice2["url"] += "?2"
pk.append(twice2)
bad = entry("relbad")
bad["sha256"] = "0" * 64           # the download will not match: the plan stops there
pk.append(bad)

for zip_path, idx_path, pid in ((a.hardcode, a.hardcode_index, "hardcode"), (a.bishop, a.bishop_index, "bishop")):
    idx = json.loads(Path(idx_path).read_text())
    e = next(p for p in idx["packages"] if p["id"] == pid)
    data = Path(zip_path).read_bytes()
    if hashlib.sha256(data).hexdigest() != e["sha256"]:
        sys.exit(f"{zip_path} is not the archive {idx_path} lists")
    (out / "cache" / f"{pid}.zip").write_bytes(data)
    pk.append(e)
    if pid == "hardcode":
        limpet = next(p for p in idx["packages"] if p["id"] == "limpet")
        with urllib.request.urlopen(limpet["url"], timeout=120) as r:
            ld = r.read()
        if hashlib.sha256(ld).hexdigest() != limpet["sha256"]:
            sys.exit("the Limpet release does not match its catalogue entry")
        (out / "cache" / "limpet.zip").write_bytes(ld)
        pk.append(limpet)

(out / "idx.json").write_text(json.dumps({"schema": 1, "source": "test", "packages": pk}, indent=1) + "\n")
shutil.copy(a.apkg, out / "apkg")
if a.pkgmanager:
    shutil.copy(a.pkgmanager, out / "PkgManager")
shutil.copy(Path(__file__).with_name("relations.script"), out / "relations.script")
print(f"{out}: {len(pk)} catalogue entries")
