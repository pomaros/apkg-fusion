#!/usr/bin/env python3
"""Import the AROS Archives catalogue into a list of CANDIDATES.

This is the import half of the pipeline. It does not produce the repository
index; `mkindex.py` does that, and it reads approved manifests rather than the
catalogue. The separation exists because the first version of this tool
generated the index straight from the catalogue, which meant hand-written
dependencies in a manifest could never reach the index at all.

    catalogue  --import-->  index/candidates.json + skeletons in index/candidates/
    approved   --mkindex->  index.json          (both in arospkg-index)

What this writes:

  index/candidates.json          every offered catalogue entry, with the
                                 alternatives for each (id, arch) listed, so a
                                 human can see what was NOT chosen
  index/candidates/<id>.<arch>.toml  a skeleton, only if neither a skeleton
                                 here nor an approved manifest in
                                 arospkg-index/manifests/ has that name.
                                 An existing manifest is never modified.

Nothing here is authoritative. `status = "skeleton"` means exactly that: the
entry has not been reviewed and must not be installed.
"""

import argparse
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import catalogue as cat


SKELETON = '''# SKELETON -- imported from the archives catalogue, not reviewed.
#
# Nothing here may be installed while status = "skeleton". Approving means:
#   * checking that `id` is the right stable name for this program. The
#     catalogue bakes versions into filenames, so the suggestion may well be
#     wrong (python-2.5.2 and python-2.7.18 are one program, not two).
#   * downloading the archive, recording its sha256, and looking inside it to
#     fill in `subdir` and any install rules.
#   * establishing dependencies and setting depends_checked = true. An empty
#     `depends` with depends_checked = false means NOT YET LOOKED AT; it does
#     not mean the package has no dependencies.
#   * confirming this upload is the one we want. `selected_by` records why the
#     importer picked it; see `alternatives` in candidates.json for the rest.

status          = "skeleton"
id              = "{id}"
version         = {version}
summary         = {summary}
arch            = "{arch}"
abi             = {abi}
category        = "{category}"
url             = "{url}"
size            = {size}
upload_date     = "{date}"
selected_by     = "{selected_by}"

sha256          = ""        # required before this package can be installed
depends         = []
depends_checked = false     # false = not investigated, NOT "no dependencies"
kind            = "app"     # app | lib
subdir          = ""        # directory inside the archive; "" = archive root
'''


def toml_str(v):
    return json.dumps(v if v is not None else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalogue", help="local FULLINDEX.readme instead of fetching")
    ap.add_argument("--out", default="index", help="output directory (default: index)")
    ap.add_argument("--approved", default="../arospkg-index/manifests",
                    help="approved manifests; a candidate already approved there is skipped")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    if args.catalogue:
        raw = Path(args.catalogue).read_bytes()
    else:
        with urllib.request.urlopen(cat.CATALOGUE, timeout=60) as r:
            raw = r.read()

    entries, bad = cat.parse(raw.decode("latin-1"))

    counts = {}
    for e in entries:
        counts[e["arch"] or "untagged"] = counts.get(e["arch"] or "untagged", 0) + 1

    # Offered means "could run on one of the ABIs we build for". Both are
    # catalogued; the client refuses the ones its own ABI cannot run.
    offered = [e for e in entries
               if e["arch"] in cat.SUPPORTED_ARCH and e["abi"] in cat.SUPPORTED_ABI]
    wrong_abi = [e for e in entries
                 if e["arch"] in cat.SUPPORTED_ARCH and e["abi"] not in cat.SUPPORTED_ABI]
    by_abi = {}
    for e in offered:
        by_abi[e["abi"]] = by_abi.get(e["abi"], 0) + 1

    # Group by (id, arch). Keying on id alone drops the second architecture:
    # super_trevor_land ships x86_64 and aarch64 and an earlier version of this
    # code silently discarded the ARM build.
    groups, invalid = {}, []
    for e in offered:
        e["id"] = cat.suggest_id(e["filename"])
        if not cat.ID_RE.match(e["id"]):
            invalid.append(e); continue
        groups.setdefault((e["id"].lower(), e["arch"]), []).append(e)

    out = Path(args.out)
    (out / "candidates").mkdir(parents=True, exist_ok=True)

    candidates, created, existing = [], 0, 0
    for (pid, arch), group in sorted(groups.items()):
        # Newest upload wins as a SUGGESTION only. It can be a prerelease, or an
        # older version re-uploaded later -- protrekkr's newest x86_64 upload is
        # "2.8.3PRE", newer than the 2.8.2 release beside it. The alternatives
        # are recorded so approval is an informed choice, not a rubber stamp.
        group.sort(key=lambda x: x["date"], reverse=True)
        chosen = group[0]
        reason = "only-candidate" if len(group) == 1 else "newest-upload"

        candidates.append({
            "id": chosen["id"], "arch": arch,
            "chosen": {k: chosen[k] for k in
                       ("filename", "category", "version", "date", "size", "url", "summary", "abi")},
            "selected_by": reason,
            "alternatives": [{k: g[k] for k in ("filename", "category", "version", "date", "size")}
                             for g in group[1:]],
        })

        manifest = out / "candidates" / f"{chosen['id']}.{arch}.toml"
        if manifest.exists() or (Path(args.approved) / manifest.name).exists():
            existing += 1
            continue
        manifest.write_text(SKELETON.format(
            id=chosen["id"], version=toml_str(chosen["version"] or None),
            summary=toml_str(chosen["summary"]), arch=arch,
            abi=toml_str(chosen["abi"]), category=chosen["category"],
            url=chosen["url"], size=chosen["size"], date=chosen["date"],
            selected_by=reason), encoding="utf-8")
        created += 1

    (out / "candidates.json").write_text(
        json.dumps({"schema": 1, "source": cat.CATALOGUE,
                    "candidates": candidates}, indent=2) + "\n", encoding="utf-8")

    print(f"catalogue entries      {len(entries)}")
    if bad:
        print(f"unparseable lines      {len(bad)}   (first: line {bad[0][0]})")
    if args.stats:
        for arch, n in sorted(counts.items(), key=lambda kv: -kv[1]):
            print(f"  {arch:<12} {n:>5}" + ("  <- offered" if arch in cat.SUPPORTED else ""))
    multi = sum(1 for c in candidates if c["alternatives"])
    if wrong_abi:
        print(f"right arch, no ABI tag {len(wrong_abi)}")
    print(f"offered                {len(offered)}")
    for a in sorted(by_abi):
        who = "mainline" if a == "v1" else "distributions, e.g. AROS One"
        print(f"  ABI{a:<4}              {by_abi[a]:>4}   ({who})")
    print(f"candidates (id,arch)   {len(candidates)}")
    print(f"  with alternatives    {multi}   (upload date chose; needs review)")
    if invalid:
        print(f"  unusable ids         {len(invalid)}")
    print(f"skeletons created      {created}")
    print(f"manifests left alone   {existing}")
    print(f"\nwritten {out/'candidates.json'}")
    print("Candidates are not packages. Run mkindex.py to build the index from")
    print("APPROVED manifests; skeletons are excluded until reviewed.")


if __name__ == "__main__":
    main()
