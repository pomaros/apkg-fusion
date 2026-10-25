#!/usr/bin/env python3
"""Relations in the catalogue tools (tools/mkindex.py): what an author may
declare, how depends close over CPU, ABI and version, cycles, and which
catalogue file a package with relations goes to. Host only."""
import json, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "tools"))
import mkindex  # noqa: E402

fails = []
BASE = dict(status="approved", id="app", version="1.0", arch="x86_64", abi="v11",
            url="https://example.invalid/app.zip", size=10, sha256="a" * 64, kind="app",
            depends=[], depends_checked=True)


def refused(change, want, name):
    got = mkindex.check({**BASE, **change})
    if not any(want in p for p in got):
        fails.append(f"{name}: expected {want!r}, got {got}")


def passes(change, name):
    got = mkindex.check({**BASE, **change})
    if got:
        fails.append(f"{name}: expected to pass, got {got}")


passes({"depends": ["lib", "lib2 >= 1.2.3"]}, "depends with and without a minimum")
refused({"depends": ["lib > 1"]}, "is not \"id\" or", "only >= is a constraint")
refused({"depends": ["lib >= 1.0b"]}, "is not \"id\" or", "a minimum must be dotted numbers")
refused({"depends": ["Lib"]}, "is not \"id\" or", "ids are lower case")
refused({"depends": ["app"]}, "names the package itself", "no dependency on itself")
passes({"recommends": [{"id": "limpet", "min_version": "0.4.1", "reason": "full screen"}]}, "a recommendation")
refused({"recommends": [{"id": "limpet"}]}, "reason missing", "a recommendation says what it adds")
refused({"recommends": [{"id": "limpet", "reason": 'say "hi"'}]}, "plain ASCII", "reason plain ASCII")
refused({"recommends": [{"id": "limpet", "reason": "x", "url": "y"}]}, "unknown key", "no unknown keys")
refused({"recommends": [{"id": "limpet", "min_version": "new", "reason": "x"}]}, "dotted numbers", "min_version numeric")
passes({"conflicts": ["other"], "replaces": ["old"]}, "conflicts and replaces")
passes({"conflicts": ["old"], "replaces": ["old"]}, "replaces and conflicts with one package agree")
refused({"depends": ["x"], "conflicts": ["x"]}, "both needed and refused", "needed and refused")
refused({"depends": ["x"], "replaces": ["x"]}, "both needed and refused", "needed and replaced")
refused({"conflicts": ["x", "x"]}, "named twice", "named twice")
refused({"replaces": ["App"]}, "not a legal package id", "replaces ids are ids")
refused({"conflicts": [f"p{i}" for i in range(17)]}, "at most 16", "a bounded list")
if mkindex.client_features({**BASE, "recommends": [{"id": "x", "reason": "y"}]}):
    fails.append("a recommendation alone needs no new client")
if mkindex.client_features({**BASE, "replaces": ["x"]}) != ["relations"]:
    fails.append("replaces needs the relations feature")


def toml(m):
    out = []
    for k, v in m.items():
        if k != "recommends":
            out.append(f"{k} = {json.dumps(v)}")
    for r in m.get("recommends", []):
        out.append("[[recommends]]")
        out += [f"{a} = {json.dumps(b)}" for a, b in r.items()]
    return "\n".join(out) + "\n"


def generate(manifests):
    d = Path(tempfile.mkdtemp())
    (d / "m").mkdir()
    for m in manifests:
        (d / "m" / mkindex.variant_file(m)).write_text(toml(m))
    r = subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(d / "m"),
                        "--out-dir", str(d), "--no-archive-check", "--verbose"], capture_output=True, text=True)
    load = lambda n: [(p["id"], p["abi"]) for p in json.loads((d / n).read_text())["packages"]] if (d / n).exists() else None
    return r, load("index-v3.json"), load("index-v2.json")


M = lambda **kw: {**BASE, "url": f"https://e.invalid/{kw.get('id', 'app')}-{kw.get('abi', 'v11')}.zip", **kw}
r, v3, v2 = generate([M(id="app", depends=["lib >= 1.2"]), M(id="lib", version="1.3"),
                      M(id="tool", recommends=[{"id": "lib", "reason": "faster"}])])
if r.returncode or v3 != [("app", "v11"), ("lib", "v11"), ("tool", "v11")]:
    fails.append(f"index-v3.json has every package: {r.stdout}{r.stderr} {v3}")
if v2 != [("lib", "v11"), ("tool", "v11")]:
    fails.append(f"index-v2.json, for 0.4 and 0.5, leaves out what needs relations, keeps recommends: {v2}")
r, v3, v2 = generate([M(id="app", depends=["lib"]), M(id="lib", abi="v1")])
if r.returncode == 0 or "not in the index for x86_64/v11" not in r.stdout:
    fails.append(f"a dependency must be for the same ABI: {r.returncode} {r.stdout}")
r, v3, v2 = generate([M(id="app", depends=["lib >= 2"]), M(id="lib", version="1.3")])
if r.returncode == 0 or "lib >= 2 (the index has 1.3)" not in r.stdout:
    fails.append(f"a dependency's minimum must be published: {r.stdout}")
r, v3, v2 = generate([M(id="a", depends=["b"]), M(id="b", depends=["a"])])
if r.returncode == 0 or "a cycle" not in r.stdout:
    fails.append(f"a cycle is refused: {r.stdout}")

r, v3, v2 = generate([M(id="top", depends=["b"]), M(id="b", depends=["c"]), M(id="c", depends=["b"])])
if r.returncode == 0 or "top.x86_64.toml: depends on b" not in r.stdout:
    fails.append(f"what depends on a cycle is refused too, not published with a missing dependency: {r.stdout}")
refused({"conflicts": ["a" * 64]}, "at most 63", "relation ids within what the client keeps")
refused({"depends": ["lib >= " + ".".join(["123456789"] * 8)]}, "at most 63", "a minimum version within what the client keeps")

# index.json, for clients up to 0.3.2, never gets a package that needs relations,
# even one it listed before the package gained them.
d = Path(tempfile.mkdtemp()); (d / "m").mkdir()
(d / "index.json").write_text(json.dumps({"schema": 1, "packages": [{"id": "app", "arch": "x86_64", "abi": "v11"}]}))
(d / "m" / "app.x86_64.v11.toml").write_text(toml(M(id="app", replaces=["old"])))
subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(d / "m"), "--out-dir", str(d),
                "--no-archive-check"], capture_output=True, text=True)
if [p["id"] for p in json.loads((d / "index.json").read_text())["packages"]]:
    fails.append("index.json keeps no package that needs relations")

# Large members: an entry whose review record says one member unpacks to more
# than 64 MiB needs the "large-members" feature and stays out of index-v2.json,
# which clients before 0.6 read and whose limit refuses it.
import review as rv  # noqa: E402
d = Path(tempfile.mkdtemp()); (d / "m").mkdir(); (d / "rv").mkdir()
for mm in (M(id="big"), M(id="small")):
    (d / "m" / mkindex.variant_file(mm)).write_text(toml(mm))
    rec = {"schema": 1, "suite": rv.SUITE, "id": mm["id"], "arch": mm["arch"], "abi": mm["abi"], "date": "2026-10-23",
           "sha256": mm["sha256"], "size": mm["size"], "manifest_digest": rv.manifest_digest(mm), "result": "passed",
           "sizes": {"largest_member": (90 if mm["id"] == "big" else 40) * 1024 * 1024, "unpacked": 1},
           "checks": [], "unchecked": []}
    (d / "rv" / (mkindex.variant_file(mm)[:-5] + ".json")).write_text(json.dumps(rec))
r = subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(d / "m"), "--out-dir", str(d),
                    "--no-archive-check", "--review-dir", str(d / "rv")], capture_output=True, text=True)
v3 = {p["id"]: p for p in json.loads((d / "index-v3.json").read_text())["packages"]}
v2 = [p["id"] for p in json.loads((d / "index-v2.json").read_text())["packages"]]
if r.returncode or v3["big"].get("client_features") != ["large-members"] or "client_features" in v3["small"] or v2 != ["small"]:
    fails.append(f"a member over 64 MiB needs large-members and is left out of index-v2.json: {r.stdout}{r.stderr} {v2}")
if mkindex.client_features(M(id="x"), {"sizes": {"largest_member": 64 * 1024 * 1024}}):
    fails.append("exactly 64 MiB is still within what old clients accept")

if fails:
    print("\n".join(fails)); print(f"RELATIONS: {len(fails)} failed"); sys.exit(1)
print("RELATIONS: declarations, closure, cycles, large members and the catalogue split hold")
