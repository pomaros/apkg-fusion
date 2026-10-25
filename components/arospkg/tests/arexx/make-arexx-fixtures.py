#!/usr/bin/env python3
"""The package root the ARexx acceptance run works on, as files to stage:

  IDX1  soliton revision 1 (the published archive), cls, xrick
  IDX2  the same with soliton revision 2 (one file changed, one added)
  SOLR1 SOLR2 CLSZIP   archives for the cache, so those installs never need
                       the network; xrick is fetched, because cancelling is
                       only possible while a download runs

Every archive is checked against the index that names it.

    make-arexx-fixtures.py REPO OUTDIR
"""
import hashlib, json, os, shutil, subprocess, sys

repo, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
tmp = os.path.join(out, "_sol")
subprocess.run([sys.executable, os.path.join(repo, "tests/make-real-upgrade-fixture.py"), repo, tmp],
               check=True, stdout=subprocess.DEVNULL)
sol1 = json.load(open(os.path.join(tmp, "idx-sol1.json")))["packages"][0]
sol2 = json.load(open(os.path.join(tmp, "idx-sol2.json")))["packages"][0]
rel = json.load(open(os.path.join(repo, "tests/release-index.json")))["packages"]
pub = json.load(open(os.path.join(repo, "tests/fixtures/index-v03.json")))["packages"]
cls = [p for p in rel if p["id"] == "cls"][0]
xrick = [p for p in pub if p["id"] == "xrick"][0]

def cached(entry, dest):
    src = os.path.join(repo, ".cache/archives", entry["url"].split("/share/", 1)[1])
    shutil.copyfile(src, dest)
    got = hashlib.sha256(open(dest, "rb").read()).hexdigest()
    assert got == entry["sha256"], f"{dest}: {got} is not the index's {entry['sha256']}"

cached(sol1, os.path.join(out, "SOLR1"))
cached(cls, os.path.join(out, "CLSZIP"))
shutil.copyfile(os.path.join(tmp, "soliton-r2.zip"), os.path.join(out, "SOLR2"))
assert hashlib.sha256(open(os.path.join(out, "SOLR2"), "rb").read()).hexdigest() == sol2["sha256"]
for name, sol in (("IDX1", sol1), ("IDX2", sol2)):
    json.dump({"schema": 1, "source": "arexx acceptance", "packages": [sol, cls, xrick]},
              open(os.path.join(out, name), "w"), indent=1)
shutil.rmtree(tmp)
print("fixtures in", out)
