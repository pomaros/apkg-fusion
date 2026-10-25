#!/usr/bin/env python3
"""The payload for the `apkg run` tests on an AROS guest.

    make-run-fixtures.py OUT_DIR --apkg BINARY [--folio ZIP] [--grafx2 LHA] [--grep ZIP]

OUT_DIR gets:
  apkg, where, tasks          the client, a program that reports how it was
                              started (tests/launch-proto/where.c), a task
                              list probe
  idx.json                    a catalogue: folio, grafx2 and grep with the
                              [launch] tables of their catalogue manifests,
                              in the shape tools/mkindex.py writes
  idx-nolaunch.json           the same without any launch table
  cache/                      the three archives, named as apkg caches them,
                              so the install needs no network
  pages12.pdf                 a document for Folio
  cases/<name>/               application drawers for the refusals: no
                              description, missing program, a path out of
                              the drawer, another CPU in the description,
                              another CPU in the file, Workbench without an
                              icon, an unknown key; and "My Tool", valid,
                              with spaces in its paths
  run1.script                 the guest script; results to RESULTS:r1-*
The archives are copied as they are: their SHA-256 is checked against the
catalogue entries here, as apkg checks them there.
"""
import argparse
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
import mkindex  # noqa: E402

LAUNCH = {
    "folio": {"executable": "Folio", "mode": "workbench"},
    "grafx2": {"executable": "GrafX2", "mode": "workbench"},
    "grep": {"executable": "Grep", "mode": "shell"},
}


def tool_icon(stack, tooltypes):
    """A classic one-plane tool icon, as tools/make-icon.py writes them."""
    w, h = 16, 8
    gadget = struct.pack(">LhhhhHHHLLLLLHL", 0, 0, 0, w, h + 1, 4, 3, 1, 1, 0, 0, 0, 0, 0, 1)
    disk = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBLLllLLl", 3, 0, 0, 1 if tooltypes else 0, -0x80000000, -0x80000000, 0, 0, stack)
    image = struct.pack(">hhhhhLBBL", 0, 0, w, h, 1, 1, 1, 0, 0)
    data = disk + image + b"\xff\xff" * h
    if tooltypes:
        data += struct.pack(">L", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            b = t.encode() + b"\0"
            data += struct.pack(">L", len(b)) + b
    return data


def app_json(id_, arch="x86_64", abi="v11", launch=None, extra=None):
    m = {"id": id_, "version": "1.0", "arch": arch, "abi": abi, "requires_system": [],
         "launch": launch or {"executable": "where", "mode": "shell"}}
    d = mkindex.app_description(m)
    if extra:
        d["launch"].update(extra)
    return json.dumps(d, indent=2) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--apkg", required=True)
    ap.add_argument("--where", default=str(ROOT / "build/proto/where"))
    ap.add_argument("--tasks", default=str(ROOT / "build/proto/tasks"))
    ap.add_argument("--i386-binary", default=str(ROOT / "build/rc/0.4.2/i386-aros-v0/apkg"),
                    help="any i386 ELF program, for the wrong-CPU file case")
    ap.add_argument("--folio", default=str(Path.home() / "Work/AROS-dev/aros-foliopdf/dist/Folio-0.4.1-aros1.x86_64-aros-v11.zip"))
    ap.add_argument("--grafx2", default=str(Path.home() / "Work/AROS-dev/aros-grafx2/dist/GrafX2-2.9-r5.x86_64-aros-v11.lha"))
    ap.add_argument("--grep", default=str(ROOT / "build/fixtures-run/grep.x86_64-aros-v11.zip"))
    ap.add_argument("--pdf", default=str(Path.home() / "Work/AROS-dev/aros-foliopdf/tests/fixtures/pages12.pdf"))
    ap.add_argument("--index", default=str(ROOT.parent / "arospkg-index/index-v2.json"))
    a = ap.parse_args()

    out = Path(a.out)
    if out.exists():
        sys.exit(f"{out} exists; give a new directory")
    (out / "cache").mkdir(parents=True)
    for src, name in ((a.apkg, "apkg"), (a.where, "where"), (a.tasks, "tasks")):
        shutil.copy(src, out / name)
        (out / name).chmod(0o755)
    shutil.copy(a.pdf, out / "pages12.pdf")

    index = json.loads(Path(a.index).read_text())
    pkgs = []
    for id_, archive in (("folio", a.folio), ("grafx2", a.grafx2), ("grep", a.grep)):
        e = next(p for p in index["packages"] if p["id"] == id_ and p["arch"] == "x86_64" and p.get("abi") == "v11")
        data = Path(archive).read_bytes()
        if hashlib.sha256(data).hexdigest() != e["sha256"] or len(data) != e["size"]:
            sys.exit(f"{archive} is not the archive the catalogue lists for {id_}")
        ext = e["url"].rsplit(".", 1)[-1].lower()
        (out / "cache" / f"{id_}.{ext}").write_bytes(data)
        pkgs.append(e)
    with_launch = [{**p, "launch": mkindex.launch_normal(LAUNCH[p["id"]])} for p in pkgs]
    for name, ps in (("idx.json", with_launch), ("idx-nolaunch.json", pkgs)):
        (out / name).write_text(json.dumps({"schema": 1, "source": "run fixtures", "packages": ps}, indent=2) + "\n")

    where = Path(a.where).read_bytes()
    i386 = Path(a.i386_binary).read_bytes()
    cases = out / "cases"

    def drawer(name, desc=None, program="where", body=where, icon=None):
        d = cases / name
        d.mkdir(parents=True)
        if body is not None:
            (d / program).write_bytes(body)
            (d / program).chmod(0o755)
        if icon is not None:
            (d / (program + ".info")).write_bytes(icon)
        if desc is not None:
            (d / "application.json").write_text(desc)
        return d

    my = drawer("My Tool", app_json("mytool", launch={"executable": "My where", "mode": "shell",
                                                        "working_directory": "Data Files"}),
                program="My where", icon=tool_icon(262144, ["DONOTWAIT"]))
    (my / "Data Files").mkdir()
    (my / "Data Files" / "note.txt").write_text("data\n")
    drawer("wbtool", app_json("wbtool", launch={"executable": "where", "mode": "workbench"}),
           icon=tool_icon(200000, ["WHERE_TEST=yes"]))
    drawer("nodesc")
    drawer("missing", app_json("missing", launch={"executable": "Nope", "mode": "shell"}))
    drawer("badpath", app_json("badpath").replace('"where"', '"../where"'))
    drawer("i386desc", app_json("i386desc", arch="i386", abi="v0"))
    drawer("elfwrong", app_json("elfwrong"), body=i386)
    drawer("wbnoicon", app_json("wbnoicon", launch={"executable": "where", "mode": "workbench"}))
    drawer("unknownkey", app_json("unknownkey", extra={"arguments": "-x"}))

    shutil.copy(HERE / "run1.script", out / "run1.script")
    print(f"written {out}")


if __name__ == "__main__":
    main()
