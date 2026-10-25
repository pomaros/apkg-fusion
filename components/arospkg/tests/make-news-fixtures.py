#!/usr/bin/env python3
"""The payload for what is new after `apkg update`, on an AROS guest.

    make-news-fixtures.py OUTDIR PUBLISHED_INDEX_V2 --apkg BINARY [--pkgmanager BINARY]

The new catalogue is the real one: the guest fetches it from the published
URL, through the client's own download and TLS. The catalogue it replaces
is made here from the same file, so the differences are known in advance:

  limpet, arospkg   left out: new applications (first published after the
                    old catalogue's date, 2026-10-01)
  xrick             left out: a new application that is back (first
                    published 2026-09-19, before that date)
  folio             only its i386 build kept: a new build for x86_64/v11
  zgloom, scopagame older versions: updated
  everything else   unchanged

OUTDIR gets apkg (and PkgManager), old.json, news.script and expect.json
(the classification the host grades against). The guest script writes every
result to RESULTS:<tag>-NN-*.txt.
"""
import argparse, copy, json, shutil, sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("published")
ap.add_argument("--apkg", required=True)
ap.add_argument("--pkgmanager")
a = ap.parse_args()
out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
idx = json.loads(Path(a.published).read_text())
pk = idx["packages"]
ids = {p["id"] for p in pk}
for need in ("limpet", "arospkg", "xrick", "folio", "zgloom", "scopagame"):
    if need not in ids:
        sys.exit(f"the published catalogue has no {need}")
if not any("first_added" in p for p in pk):
    sys.exit("the published catalogue carries no dates: publish history.json first")

old = []
for p in pk:
    if p["id"] in ("limpet", "arospkg", "xrick"):
        continue
    if p["id"] == "folio" and p["arch"] == "x86_64":
        continue
    q = copy.deepcopy(p)
    if p["id"] == "zgloom" and p["arch"] == "x86_64":
        q["version"] = "052026v2"
    if p["id"] == "scopagame" and p["arch"] == "x86_64":
        q["version"] = "1.0"
    old.append(q)
old_text = json.dumps({**idx, "packages": old}, indent=1) + "\n"
(out / "old.json").write_text(old_text)
# The record of the refresh that fetched it, as apkg 0.6 writes it: the
# update compares with the catalogue of that date (2026-10-01), whatever
# the file's own date on the guest.
import hashlib
(out / "news.json").write_text(json.dumps({
    "schema": 1, "mode": "last-update",
    "catalogue": "https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/main/index-v2.json",
    "refreshed": "2026-10-01 12:00", "compared_with": "",
    "catalogue_sha256": hashlib.sha256(old_text.encode()).hexdigest(),
    "recorded": True, "current": True, "first_catalogue": False, "first_reason": "",
    "counts": {"new_applications": 0, "new_builds": 0, "updated": 0, "recently_added": 0, "unknown_dates": 0},
    "items": []}, indent=1) + "\n")
expect = {"new-application": ["arospkg", "limpet", "xrick"], "returned": ["xrick"],
          "new-build": ["folio"], "updated": ["scopagame", "zgloom"]}
(out / "expect.json").write_text(json.dumps(expect, indent=1) + "\n")
shutil.copy(a.apkg, out / "apkg")
if a.pkgmanager:
    shutil.copy(a.pkgmanager, out / "PkgManager")
shutil.copy(Path(__file__).with_name("news.script"), out / "news.script")
print(f"{out}: old.json with {len(old)} of {len(pk)} entries; expected {expect}")
