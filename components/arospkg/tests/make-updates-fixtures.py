#!/usr/bin/env python3
"""Fixtures and the guest script for the updates model on AROS: apkg
outdated (text, --all, --plain, --json, redirected), the count after an
update, upgrade --all with a conflict, and the state after rollback and
remove. No network: every archive is placed in the package cache with the
hash the catalogue expects.

    make-updates-fixtures.py OUTDIR RUNID [ARCH ABI]

OUTDIR becomes a payload directory: apkg is copied in by the caller. The
guest extracts it to SYS:UPDT (see the header of updates.script) and the
script writes every result to RESULTS:upd-<RUNID>-*.txt, one file per step.

Installed (catalogue 1)            Catalogue 2 says           expected state
  upa 1.0 r1                         1.1 r1                     available
  upb 1.0 r1                         1.0 r2                     available (but the user changed Prog: conflict)
  upc 1.0 r1                         1.0 r1                     current
  upd 2.0 r1                         1.5 r1                     newer-installed
  upe 1.0 r1                         1.1, needs a missing lib   blocked
  upf 1.0a r1                        1.0b r1                    incomparable
  upg 1.0 r1                         (absent)                   not-in-catalogue
  uph 1.0 r1                         1.1 twice for this target  ambiguous
  upi 1.0 r1                         1.1 for the other ABI only other-target
"""
import hashlib, json, os, sys, zipfile

out, run = sys.argv[1], sys.argv[2]
arch = sys.argv[3] if len(sys.argv) > 3 else "x86_64"
abi = sys.argv[4] if len(sys.argv) > 4 else "v11"
other_abi = "v1" if abi == "v11" else "v11"
os.makedirs(os.path.join(out, "a"), exist_ok=True)

def sha(b): return hashlib.sha256(b).hexdigest()

def make_zip(name, files):
    path = os.path.join(out, "a", name)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for n, d in files.items():
            z.writestr(n, d)
    return open(path, "rb").read()

def files(pid, tag):
    d = pid.capitalize()
    return {f"{d}/Prog": f"prog {tag}\n".encode(), f"{d}/config.txt": b"cfg\n", f"{d}/ReadMe": b"same\n"}

def entry(pid, ver, rev, blob, a=arch, b=abi, req=None):
    return {"id": pid, "version": ver, "revision": rev, "summary": f"updates fixture {pid}",
            "arch": a, "abi": b, "category": "test/fixture", "url": f"https://example.invalid/{pid}-{ver}-{rev}.zip",
            "size": len(blob), "sha256": sha(blob), "depends": [], "requires_system": req or [],
            "kind": "app", "subdir": pid.capitalize()}

old = {p: make_zip(f"{p}-old.zip", files("up" + p, "old")) for p in "abcdefghi"}
new = {p: make_zip(f"{p}-new.zip", files("up" + p, "new")) for p in "abcdefghi"}
ver1 = {"a": "1.0", "b": "1.0", "c": "1.0", "d": "2.0", "e": "1.0", "f": "1.0a", "g": "1.0", "h": "1.0", "i": "1.0"}
cat1 = {"schema": 1, "source": "updates test 1", "packages": [entry("up" + p, ver1[p], 1, old[p]) for p in "abcdefghi"]}
missing = [{"type": "library", "id": "arospkg-test-missing.library"}]
cat2 = {"schema": 1, "source": "updates test 2", "packages": [
    entry("upa", "1.1", 1, new["a"]),
    entry("upb", "1.0", 2, new["b"]),
    entry("upc", "1.0", 1, old["c"]),
    entry("upd", "1.5", 1, new["d"]),
    entry("upe", "1.1", 1, new["e"], req=missing),
    entry("upf", "1.0b", 1, new["f"]),
    entry("uph", "1.1", 1, new["h"]),
    dict(entry("uph", "1.1", 1, new["h"]), url="https://example.invalid/uph-other.zip"),
    entry("upi", "1.1", 1, new["i"], b=other_abi),
]}
# For the Ctrl-C test of upgrade --all: two updates that both apply cleanly.
cat3 = {"schema": 1, "source": "updates test 3", "packages": [
    entry("upa", "1.1", 1, new["a"]), entry("upc", "1.1", 1, new["c"])]}
for name, cat in (("idx1.json", cat1), ("idx2.json", cat2), ("idx3.json", cat3)):
    with open(os.path.join(out, name), "w") as f:
        json.dump(cat, f, indent=1)
with open(os.path.join(out, "badidx.json"), "w") as f:
    f.write('{"schema": 1, "packages": [ {"id": "upa", ')

H = {"a-new": sha(files("upa", "new")["Upa/Prog"]), "a-old": sha(files("upa", "old")["Upa/Prog"]),
     "b-user": sha(b"prog old\nuser edit\n"), "cfg-user": sha(b"cfg\nuser edit\n")}

R = f"RESULTS:upd-{run}"
ROOT = "SYS:PkgUpd"
A = f"SYS:UPDT/apkg --root {ROOT}"
L = [f"""; The updates model on AROS, run {run}. Written by tests/make-updates-fixtures.py.
; Extract the payload first:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d SYS:UPDT
; then:                       Execute SYS:UPDT/updates.script
; Each step writes its own RESULTS: file; nothing is printed to the console.
FAILAT 1000
Protect SYS:UPDT/apkg +e
Delete {ROOT} "SYS:Pkg Upd S" SYS:PkgUpdN SYS:PkgUpdB SYS:PkgUpdG ALL FORCE QUIET
MakeDir {ROOT}
MakeDir {ROOT}/cache
"""]
def step(name, cmd):
    L.append(f'{cmd} >{R}-{name}.txt')
    L.append(f'Echo >>{R}-{name}.txt "rc=$RC"')

for p in "abcdefghi":
    L.append(f"Copy SYS:UPDT/a/{p}-old.zip {ROOT}/cache/up{p}.zip")
    L.append(f"Copy SYS:UPDT/a/{p}-new.zip {ROOT}/cache/up{p}.new.zip")
L.append(f"Echo >{R}-install.txt \"install from catalogue 1\"")
for p in "abcdefghi":
    L.append(f"{A} --plain --index SYS:UPDT/idx1.json install up{p} >>{R}-install.txt")
    L.append(f'Echo >>{R}-install.txt "rc=$RC"')
step("status-cat1", f"{A} --index SYS:UPDT/idx1.json --plain outdated --all")
L.append("; the new catalogue arrives (as apkg update would publish it), and the user edits two files")
L.append(f"Copy SYS:UPDT/idx2.json {ROOT}/db/index.json")
L.append(f'Echo >>{ROOT}/Upa/config.txt "user edit"')
L.append(f'Echo >>{ROOT}/Upb/Prog "user edit"')
step("outdated", f"{A} outdated")
step("outdated-all", f"{A} outdated --all")
step("outdated-plain", f"{A} --plain outdated")
step("outdated-json", f"{A} --json outdated")
step("search", f"{A} search")
step("search-up", f"{A} search upa")
step("show-upa", f"{A} show upa")
step("dry-all", f"{A} --dry-run upgrade --all")
step("upgrade-all", f"{A} upgrade --all")
step("hash-a-prog", f"SYS:UPDT/apkg assert-hash {ROOT}/Upa/Prog {H['a-new']}")
step("hash-a-cfg", f"SYS:UPDT/apkg assert-hash {ROOT}/Upa/config.txt {H['cfg-user']}")
step("hash-b-prog", f"SYS:UPDT/apkg assert-hash {ROOT}/Upb/Prog {H['b-user']}")
step("after-upgrade", f"{A} --json outdated")
step("rollback-a", f"{A} rollback upa")
step("hash-a-back", f"SYS:UPDT/apkg assert-hash {ROOT}/Upa/Prog {H['a-old']}")
step("after-rollback", f"{A} --json outdated")
step("remove-b", f"{A} remove upb")
step("after-remove", f"{A} --json outdated")
L.append("; no catalogue, and a catalogue that is not one")
L.append("MakeDir SYS:PkgUpdN")
step("nocat", "SYS:UPDT/apkg --root SYS:PkgUpdN outdated")
step("nocat-json", "SYS:UPDT/apkg --root SYS:PkgUpdN --json outdated")
L.append("MakeDir SYS:PkgUpdB\nMakeDir SYS:PkgUpdB/db")
L.append("Copy SYS:UPDT/badidx.json SYS:PkgUpdB/db/index.json")
step("badcat", "SYS:UPDT/apkg --root SYS:PkgUpdB outdated")
L.append("; a root with a space: the commands apkg prints must carry it, quoted")
L.append('MakeDir "SYS:Pkg Upd S"\nMakeDir "SYS:Pkg Upd S/cache"')
L.append('Copy SYS:UPDT/a/a-old.zip "SYS:Pkg Upd S/cache/upa.zip"')
L.append('SYS:UPDT/apkg --root "SYS:Pkg Upd S" --index SYS:UPDT/idx1.json --plain install upa >NIL:')
L.append('Copy SYS:UPDT/idx2.json "SYS:Pkg Upd S/db/index.json"')
step("space-root", 'SYS:UPDT/apkg --root "SYS:Pkg Upd S" outdated')
step("usage", "SYS:UPDT/apkg outdated extra")
L.append("; prepared for the interactive Ctrl-C test of upgrade --all (see the README of the run):")
L.append(";   SYS:UPDT/apkg --root SYS:PkgUpdG --slow 1500 upgrade --all   then Ctrl-C while upa is being installed")
L.append("MakeDir SYS:PkgUpdG\nMakeDir SYS:PkgUpdG/cache")
for p in "ac":
    L.append(f"Copy SYS:UPDT/a/{p}-old.zip SYS:PkgUpdG/cache/up{p}.zip")
    L.append(f"SYS:UPDT/apkg --root SYS:PkgUpdG --index SYS:UPDT/idx1.json --plain install up{p} >NIL:")
    L.append(f"Copy SYS:UPDT/a/{p}-new.zip SYS:PkgUpdG/cache/up{p}.new.zip")
L.append("Copy SYS:UPDT/idx3.json SYS:PkgUpdG/db/index.json")
step("group-before", "SYS:UPDT/apkg --root SYS:PkgUpdG --plain outdated")
L.append(f'Echo >{R}-done.txt "done"')
with open(os.path.join(out, "updates.script"), "w") as f:
    f.write("\n".join(L) + "\n")
print(json.dumps(H, indent=1))
