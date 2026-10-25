#!/usr/bin/env python3
"""Assemble a self-update test run: what relserver.py serves and what the
pool machine gets staged.

    make-stage.py RUNID REL060 B1 R2 R3 CERTDIR RELDIR STAGEDIR

REL060 is the published arospkg 0.6.0 x86_64 ABIv11 archive. B1, R2 and R3
are the output directories of make-test-release.sh for 0.6.99-sutest1 (the
new code, older than the test release), 0.7.0-sutest2 and 0.7.1-sutest3.

RELDIR gets the release directories relserver.py serves (r060, b1, r2, r3,
and r2 again as bad, cut, slow and i386only for the failures), and slowidx
for a catalogue refresh slow enough to be caught in the act. STAGEDIR gets
the archives, the test CA, catalogues for managed installs, a "user's"
PkgManager icon, and the guest scripts with every expected hash written in.
RUNID prefixes every result file name, so no run overwrites another's.
"""
import hashlib, json, os, shutil, struct, sys, zipfile

run, rel060, b1, r2, r3, certdir, reldir, stage = sys.argv[1:9]
here = os.path.dirname(os.path.abspath(__file__))
T = "x86_64-aros-v11"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def archive(d):
    z = [f for f in os.listdir(d) if f.endswith(".zip")]
    assert len(z) == 1, d
    return os.path.join(d, z[0])


def members(zpath):
    with zipfile.ZipFile(zpath) as z:
        top = z.namelist()[0].split("/")[0]
        get = lambda n: z.read(top + "/" + n)
        return top, {k: sha(get(n)) for k, n in
                     (("A", "apkg"), ("P", "PkgManager"), ("B", "BUILD.txt"),
                      ("J", "application.json"), ("I", "PkgManager.info"))}, get("apkg")


def release_dir(name, zpath, sums=None):
    d = os.path.join(reldir, name)
    os.makedirs(d, exist_ok=True)
    shutil.copy(zpath, d)
    data = open(zpath, "rb").read()
    line = "%s  %s\n" % (sha(data), os.path.basename(zpath))
    open(os.path.join(d, "SHA256SUMS"), "w").write(sums if sums is not None else line)
    return d


z060, zb1, zr2, zr3 = rel060, archive(b1), archive(r2), archive(r3)
top060, h060, _ = members(z060)
topb1, hb1, apkg_b1 = members(zb1)
_, hr2, apkg_r2 = members(zr2)
_, hr3, _ = members(zr3)

shutil.rmtree(reldir, ignore_errors=True)
shutil.rmtree(stage, ignore_errors=True)
os.makedirs(stage)
release_dir("r060", z060)
release_dir("b1", zb1)
release_dir("r2", zr2)
release_dir("r3", zr3)
release_dir("cut", zr2)
release_dir("slow", zr2)
release_dir("slow3", zr3)
name2 = os.path.basename(zr2)
release_dir("bad", zr2, "%s  %s\n" % ("0" * 64, name2))
release_dir("i386only", zr2, "%s  %s\n" % (sha(open(zr2, "rb").read()),
                                           name2.replace(T, "i386-aros-v0")))
os.makedirs(os.path.join(reldir, "slowidx"))
shutil.copy(os.path.expanduser("~/Work/AROS-dev/arospkg-index/index-v3.json"),
            os.path.join(reldir, "slowidx", "index-v3.json"))


def catalogue(zpath, top, version, base):
    data = open(zpath, "rb").read()
    entry = {
        "id": "arospkg", "version": version, "summary": "Package manager for AROS",
        "arch": "x86_64", "abi": "v11", "category": "utility/system",
        "url": "https://10.0.2.2/%s/%s" % (base, os.path.basename(zpath)),
        "size": len(data), "sha256": sha(data), "depends": [],
        "requires_system": [{"type": "library", "id": l} for l in
                            ("crt.library", "stdlib.library", "muimaster.library")],
        "installs_on": [], "runs_on": [], "does_not_run_on": [], "kind": "app",
        "subdir": top, "icon": "", "install": {}, "license": "MIT",
        "launch": {"executable": "PkgManager", "mode": "workbench", "working_directory": "."},
    }
    return json.dumps({"schema": 1, "source": "self-update test", "packages": [entry]}, indent=1)


open(os.path.join(stage, "idx060.json"), "w").write(catalogue(z060, top060, "0.6.0", "r060"))
# Revision 1: a window's update of this package hands the revision on to
# the apkg that finishes it, which must find the same one.
c = json.loads(catalogue(zb1, topb1, "0.6.99-sutest1", "b1")); c["packages"][0]["revision"] = 1
open(os.path.join(stage, "idxb1.json"), "w").write(json.dumps(c, indent=1))
# For a package that changes under a waiting self-update: the same 0.6.0 as
# revision 2, and the 0.7.1-sutest3 archive catalogued as a newer 0.8.0.
c = json.loads(catalogue(z060, top060, "0.6.0", "r060")); c["packages"][0]["revision"] = 2
open(os.path.join(stage, "idx060r2.json"), "w").write(json.dumps(c, indent=1))
top3 = members(zr3)[0]
open(os.path.join(stage, "idx080.json"), "w").write(catalogue(zr3, top3, "0.8.0", "r3"))

# A user's PkgManager icon: moved on the desktop and given a tool type of
# its own, which an update must keep.
icon = bytearray(open(os.path.join(here, "..", "..", "release", "icons", "PkgManager.info"), "rb").read())
tail = struct.pack(">L", 8) + struct.pack(">L", 10) + b"DONOTWAIT\0"
assert icon.endswith(tail), "unexpected PkgManager.info layout"
icon = icon[:-len(tail)]
struct.pack_into(">ll", icon, 58, 120, 40)
tts = [b"DONOTWAIT\0", b"USERSETTING=kept\0"]
icon += struct.pack(">L", (len(tts) + 1) * 4) + b"".join(struct.pack(">L", len(t)) + t for t in tts)
open(os.path.join(stage, "user.info"), "wb").write(icon)

for z in (z060, zb1):
    shutil.copy(z, stage)
open(os.path.join(stage, "apkg-b1"), "wb").write(apkg_b1)
open(os.path.join(stage, "apkg-r2"), "wb").write(apkg_r2)
shutil.copy(os.path.join(stage, "apkg-r2"), os.path.join(stage, "hc"))
shutil.copy(os.path.join(certdir, "ca.crt"), stage)
shutil.copy(os.path.join(here, "..", "tools", "putfile"), stage)

subst = {"R": run, "IUSER": sha(bytes(icon))}
for tag, h in (("060", h060), ("1", hb1), ("2", hr2), ("3", hr3)):
    for k, v in h.items():
        subst[k + tag] = v
for t in ("setup", "shell", "managed", "cleanup"):
    text = open(os.path.join(here, t + ".script.in")).read()
    for k, v in subst.items():
        text = text.replace("{{%s}}" % k, v)
    assert "{{" not in text, t
    open(os.path.join(stage, t + ".script"), "w").write(text)
# gui.script.in holds one script per window step, each after "### <name>".
parts = (open(os.path.join(here, "gui.script.in")).read() + open(os.path.join(here, "bg.script.in")).read()).split("### ")[1:]
for part in parts:
    name, text = part.split("\n", 1)
    for k, v in subst.items():
        text = text.replace("{{%s}}" % k, v)
    assert "{{" not in text, name
    open(os.path.join(stage, name.strip() + ".script"), "w").write(text)
json.dump(subst, open(os.path.join(stage, "expect.json"), "w"), indent=1)
print(json.dumps(subst, indent=1))
