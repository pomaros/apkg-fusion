#!/usr/bin/env python3
"""Two revisions of a controlled package, the two index files that describe
them, and the guest script that upgrades, rolls back, conflicts and gets
interrupted between them.

    make-upgrade-fixtures.py OUTDIR

Revision 1 (up-r1.zip):            Revision 2 (up-r2.zip):
  Up.info      icon A                Up.info      icon B        (changed)
  Up/Prog      "prog r1"             Up/Prog      "prog r2"     (changed)
  Up/Same      "same"                Up/Same      "same"        (unchanged)
  Up/OldFile   "old"                 --                         (dropped)
  --                                 Up/NewFile   "new"         (added)
  Up/config.txt "cfg"                Up/config.txt "cfg"        (unchanged; the user edits it)

The archives are placed in the package cache by the script, so the test
needs no network: fetch_verified accepts a cached copy whose hash matches.
"""
import hashlib, json, os, sys, zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def sha(b): return hashlib.sha256(b).hexdigest()

R1 = {"Up.info": b"ICON-A" * 16, "Up/Prog": b"prog r1\n", "Up/Same": b"same\n",
      "Up/OldFile": b"old\n", "Up/config.txt": b"cfg\n"}
R2 = {"Up.info": b"ICON-B" * 16, "Up/Prog": b"prog r2\n", "Up/Same": b"same\n",
      "Up/NewFile": b"new\n", "Up/config.txt": b"cfg\n"}

def make_zip(path, files):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return open(path, "rb").read()

z1 = make_zip(os.path.join(out, "up-r1.zip"), R1)
z2 = make_zip(os.path.join(out, "up-r2.zip"), R2)

def index(rev, blob):
    return {"schema": 1, "source": "test", "packages": [{
        "id": "up", "version": "1.0", "revision": rev, "summary": "controlled upgrade fixture",
        "arch": "x86_64", "abi": "v11", "category": "test/fixture",
        "url": f"https://example.invalid/up-r{rev}.zip", "size": len(blob), "sha256": sha(blob),
        "depends": [], "requires_system": [], "kind": "app", "subdir": "Up", "icon": "Up.info"}]}

for rev, blob in ((1, z1), (2, z2)):
    with open(os.path.join(out, f"idx-r{rev}.json"), "w") as f:
        json.dump(index(rev, blob), f, indent=1)

h = {k: sha(v) for k, v in {**{"r1/" + k: v for k, v in R1.items()},
                             **{"r2/" + k: v for k, v in R2.items()}}.items()}
h["user/config"] = sha(b"cfg\nuser edit\n")
h["user/newfile"] = sha(b"new\nuser edit\n")
h["user/prog"]    = sha(b"prog r1\nuser edit\n")

ROOT = "SYS:PkgUp"
A = f"RAM:apkg --root {ROOT} --machine onetest"
def run(rid, expect, cmd, idx=None):
    i = f" --index RAM:{idx}" if idx else ""
    return f"{A}{i} --run-id {rid} --expect {expect} --report RAM:urep/{rid}.json {cmd}"
def ah(rid, path, key):
    return f"{A} assert-hash {ROOT}/{path} {h[key]}"
def absent(path, label):
    return (f"If EXISTS {ROOT}/{path}\n  Echo >>RAM:up-console.txt \"FAIL {label}: {path} exists\"\n"
            f"Else\n  Echo >>RAM:up-console.txt \"PASS {label}: {path} absent\"\nEndIf")

L = []
L.append("""; Upgrade and rollback on a controlled package, no network: both revisions
; are placed in the cache with hashes the index expects.
;     Execute PAYLOAD:S/UPGRADE
; Every command appends to RAM:up-console.txt itself.

FAILAT 1000
Delete RAM:up-console.txt QUIET
Copy PAYLOAD:APKG RAM:apkg
Copy PAYLOAD:PUTFILE RAM:putfile
Copy PAYLOAD:I/IDX1 RAM:idx-r1.json
Copy PAYLOAD:I/IDX2 RAM:idx-r2.json
Copy PAYLOAD:I/IDXV2 RAM:idx-v2.json
Copy PAYLOAD:I/IDXVDN RAM:idx-vdown.json
Copy PAYLOAD:I/IDXVEQ RAM:idx-veq.json
Copy PAYLOAD:I/IDXVBD RAM:idx-vbad.json
Protect RAM:apkg +e
Protect RAM:putfile +e
Delete SYS:PkgUp ALL FORCE QUIET
Delete RAM:urep ALL FORCE QUIET
MakeDir RAM:urep
MakeDir SYS:PkgUp
MakeDir SYS:PkgUp/cache
Copy PAYLOAD:A/UPR1 SYS:PkgUp/cache/up.zip
Copy PAYLOAD:A/UPR2 SYS:PkgUp/cache/up.new.zip
""")
def say(t): L.append(f'Echo >>RAM:up-console.txt "*N== {t} =="')
def c(cmd): L.append(f"{cmd} >>RAM:up-console.txt")

say("install revision 1")
c(run("u-install", 0, "install up", "idx-r1.json"))
c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up.info", "r1/Up.info"))
say("the user edits config.txt")
L.append(f'Echo >>{ROOT}/Up/config.txt "user edit"')
c(ah("", "Up/config.txt", "user/config"))
say("upgrade to revision 2: Prog replaced, NewFile added, OldFile removed, config kept, icon replaced")
c(run("u-upgrade", 0, "upgrade up", "idx-r2.json"))
c(ah("", "Up/Prog", "r2/Up/Prog")); c(ah("", "Up/NewFile", "r2/Up/NewFile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r2/Up.info"))
L.append(absent("Up/OldFile", "u-upgrade"))
c(f"{A} info up")
c(f"List {ROOT}/cache {ROOT}/cache/previous {ROOT}/db/previous")
say("the user edits NewFile after the upgrade")
L.append(f'Echo >>{ROOT}/Up/NewFile "user edit"')
say("rollback: Prog back to r1, OldFile back, NewFile kept as edited, config kept, icon back")
c(run("u-rollback", 0, "rollback up"))
c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up/NewFile", "user/newfile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r1/Up.info"))
say("rollback again: nothing to go back to")
c(run("u-rollback-none", 4, "rollback up"))
say("conflict: the user edits Prog, which revision 2 changes -> refused, nothing touched")
L.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")     # a rollback consumes the newer archive
L.append(f"Delete {ROOT}/Up/NewFile QUIET")
L.append(f'Echo >>{ROOT}/Up/Prog "user edit"')
c(run("u-conflict", 7, "upgrade up", "idx-r2.json"))
c(ah("", "Up/Prog", "user/prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up.info", "r1/Up.info"))
L.append(absent("Up/NewFile", "u-conflict"))
c(f"List {ROOT}/db/transactions {ROOT}/tmp")
say("restore Prog; interrupted upgrades: 9 (old tree aside), 10 (new tree in place), 5 (before commit)")
L.append(f"Delete {ROOT}/Up/Prog QUIET")
L.append(f'Echo >{ROOT}/Up/Prog "prog r1"')
c(ah("", "Up/Prog", "r1/Up/Prog"))
for point in (9, 10, 5):
    L.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")
    c(run(f"u-int{point}", 11, f"--interrupt-at {point} upgrade up", "idx-r2.json"))
    say(f"after interruption {point}: the next write open recovers; revision 1 must be intact")
    c(run(f"u-int{point}-doctor", 0, "doctor --retry"))
    c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r1/Up.info"))
    L.append(absent("Up/NewFile", f"u-int{point}"))
    c(f"List {ROOT}/db/transactions {ROOT}/tmp")
say("the real upgrade, then an interrupted rollback at 10, recovered: revision 2 must be intact")
L.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")
c(run("u-upgrade2", 0, "upgrade up", "idx-r2.json"))
c(ah("", "Up/Prog", "r2/Up/Prog"))
c(run("u-rint10", 11, "--interrupt-at 10 rollback up"))
c(run("u-rint10-doctor", 0, "doctor --retry"))
say("after the undone rollback, both archives and the previous entry are still there")
for path, label in (("cache/up.zip", "current archive"), ("cache/previous/up.zip", "previous archive"), ("db/previous/up.json", "previous registry entry")):
    L.append(f"If EXISTS {ROOT}/{path}\n  Echo >>RAM:up-console.txt \"PASS u-rint10: {label} kept\"\nElse\n  Echo >>RAM:up-console.txt \"FAIL u-rint10: {label} missing\"\nEndIf")
c(ah("", "Up/Prog", "r2/Up/Prog")); c(ah("", "Up/NewFile", "r2/Up/NewFile")); c(ah("", "Up.info", "r2/Up.info"))
L.append(absent("Up/OldFile", "u-rint10"))
say("and the rollback for real")
c(run("u-rollback2", 0, "rollback up"))
c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile"))
L.append(absent("Up/NewFile", "u-rollback2"))
say("a new upstream version, 1.0 -> 2.0: interrupted at 9 and 10 first, then recovered; 1.0 must be intact")
for point in (9, 10):
    L.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")
    c(run(f"u-ver-int{point}", 11, f"--interrupt-at {point} upgrade up", "idx-v2.json"))
    c(run(f"u-ver-int{point}-doctor", 0, "doctor --retry"))
    c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r1/Up.info"))
    L.append(absent("Up/NewFile", f"u-ver-int{point}"))
say("upgrade 1.0 -> 2.0 for real: the same plan as a revision -- Prog replaced, NewFile added, OldFile removed, the user's config kept")
L.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")
c(run("u-ver-upgrade", 0, "upgrade up", "idx-v2.json"))
c(ah("", "Up/Prog", "r2/Up/Prog")); c(ah("", "Up/NewFile", "r2/Up/NewFile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r2/Up.info"))
L.append(absent("Up/OldFile", "u-ver-upgrade"))
c(f"{A} info up")
say("an older version (0.9) is refused: no silent downgrade; 2.0 untouched")
c(run("u-ver-down", 7, "upgrade up", "idx-vdown.json"))
say("2.0.0, the same number written differently, is refused")
c(run("u-ver-equal", 7, "upgrade up", "idx-veq.json"))
say("2.1- cannot be ordered and is refused, not guessed")
c(run("u-ver-unordered", 7, "upgrade up", "idx-vbad.json"))
c(ah("", "Up/Prog", "r2/Up/Prog")); c(ah("", "Up/NewFile", "r2/Up/NewFile")); c(ah("", "Up/config.txt", "user/config"))
say("rollback from 2.0 to 1.0: the previous version's files back, the user's config kept")
c(run("u-ver-rollback", 0, "rollback up"))
c(ah("", "Up/Prog", "r1/Up/Prog")); c(ah("", "Up/OldFile", "r1/Up/OldFile")); c(ah("", "Up/config.txt", "user/config")); c(ah("", "Up.info", "r1/Up.info"))
L.append(absent("Up/NewFile", "u-ver-rollback"))
c(f"{A} info up")
L.append('Echo >>RAM:up-console.txt "== end =="')
L.append("Join RAM:urep/#?.json AS RAM:up-reports.txt")
L.append("RAM:putfile RAM:up-console.txt")
L.append("RAM:putfile RAM:up-reports.txt")
open(os.path.join(out, "UPGRADE"), "w").write("\n".join(L) + "\n")

# ---- verify and dry-run, on the same fixtures
V = []
def vsay(t): V.append(f'Echo >>RAM:vf-console.txt "*N== {t} =="')
def vc(cmd): V.append(f"{cmd} >>RAM:vf-console.txt")
def vrun(rid, expect, cmd, idx=None):
    i = f" --index RAM:{idx}" if idx else ""
    return f"{A}{i} --run-id {rid} --expect {expect} --report RAM:vrep/{rid}.json {cmd}"
V.append("""; verify and dry-run on the controlled package. Nothing a dry run prints is
; checked by the guest; the host reads the console for the words.
;     Execute PAYLOAD:S/VERIFY

FAILAT 1000
Delete RAM:vf-console.txt QUIET
Copy PAYLOAD:APKG RAM:apkg
Copy PAYLOAD:PUTFILE RAM:putfile
Copy PAYLOAD:I/IDX1 RAM:idx-r1.json
Copy PAYLOAD:I/IDX2 RAM:idx-r2.json
Protect RAM:apkg +e
Protect RAM:putfile +e
Delete SYS:PkgUp ALL FORCE QUIET
Delete RAM:vrep ALL FORCE QUIET
MakeDir RAM:vrep
MakeDir SYS:PkgUp
MakeDir SYS:PkgUp/cache
Copy PAYLOAD:A/UPR1 SYS:PkgUp/cache/up.zip
""")
vsay("install revision 1, then verify: everything as installed")
vc(vrun("v-install", 0, "install up", "idx-r1.json"))
vc(vrun("v-verify0", 0, "verify up"))
vc(f"{A} --json verify up")
vsay("edit config.txt, delete OldFile, add Extra: 1 changed, 1 missing, 1 not the package's")
V.append(f'Echo >>{ROOT}/Up/config.txt "user edit"')
V.append(f"Delete {ROOT}/Up/OldFile")
V.append(f'Echo >{ROOT}/Up/Extra "mine"')
vc(vrun("v-verify1", 0, "verify up"))
vc(f"{A} --json verify up")
vsay("dry-run remove: says what it would do, changes nothing")
vc(vrun("v-dry-remove", 0, "--dry-run remove up"))
vc(vrun("v-verify2", 0, "verify up"))
vc(f"List {ROOT}/db/installed {ROOT}/db/transactions {ROOT}/tmp")
vsay("dry-run upgrade without the new archive and without --fetch: plan incomplete, says so")
vc(vrun("v-dry-up-nofetch", 0, "--dry-run upgrade up", "idx-r2.json"))
vsay("dry-run upgrade with the new archive cached: the plan, and the conflict on Extra? no -- Extra is not in r2; the plan")
V.append(f"Copy PAYLOAD:A/UPR2 {ROOT}/cache/up.new.zip")
vc(vrun("v-dry-up", 0, "--dry-run upgrade up", "idx-r2.json"))
vc(vrun("v-verify3", 0, "verify up"))
vc(f"List {ROOT}/db/installed {ROOT}/db/transactions {ROOT}/tmp")
vsay("dry-run rollback with nothing to go back to")
vc(vrun("v-dry-rb", 0, "--dry-run rollback up"))
vsay("an interrupted upgrade, then a dry run: it must NOT recover; then doctor does")
V.append(f'Delete {ROOT}/Up/config.txt')
V.append(f'Echo >{ROOT}/Up/config.txt "cfg"')
vc(vrun("v-int9", 11, "--interrupt-at 9 upgrade up", "idx-r2.json"))
vc(vrun("v-dry-after-int", 0, "--dry-run remove up"))
vc(f"List {ROOT}/db/transactions")
vc(vrun("v-doctor", 0, "doctor --retry"))
vc(f"List {ROOT}/db/transactions")
vsay("remove for real, then dry-run install: the file list from the cached archive; without the archive, incomplete")
vc(vrun("v-remove", 0, "remove up"))
vc(vrun("v-dry-install", 0, "--dry-run install up", "idx-r1.json"))
vc(f"List {ROOT}/db/installed {ROOT}/tmp")
V.append(f"Delete {ROOT}/cache/up.zip")
vc(vrun("v-dry-install-nofetch", 0, "--dry-run install up", "idx-r1.json"))
vc(vrun("v-list", 0, "list"))
V.append('Echo >>RAM:vf-console.txt "== end =="')
V.append("Join RAM:vrep/#?.json AS RAM:vf-reports.txt")
V.append("RAM:putfile RAM:vf-console.txt")
V.append("RAM:putfile RAM:vf-reports.txt")
open(os.path.join(out, "VERIFY"), "w").write("\n".join(V) + "\n")

# Indexes that offer revision 2's archive under other upstream versions,
# edited here so the guest need not: a newer one (allowed), an older one
# (a downgrade), the same number written differently, and one that cannot
# be ordered. Revisions count per version, so 2.0 starts again at 1.
for name, version, rev in (("idx-v2.json", "2.0", 1), ("idx-vdown.json", "0.9", 9),
                           ("idx-veq.json", "2.0.0", 9), ("idx-vbad.json", "2.1-", 1)):
    v = index(rev, z2); v["packages"][0]["version"] = version
    with open(os.path.join(out, name), "w") as f: json.dump(v, f, indent=1)

print("fixtures in", out)
for k in ("r1/Up/Prog", "r2/Up/Prog", "user/config", "user/newfile", "user/prog"):
    print(f"  {k:16} {h[k][:16]}")
