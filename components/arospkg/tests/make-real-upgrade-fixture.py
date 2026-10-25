#!/usr/bin/env python3
"""A second revision of a REAL package, for the upgrade test after the
controlled one: the published soliton archive as revision 1 (the index
states no revision, which reads as 0), and a copy with one file changed and
one added as revision 2. The program itself is untouched, so it must start
after the upgrade and after the rollback exactly as it did before.

    make-real-upgrade-fixture.py REPO OUTDIR
"""
import hashlib, json, os, sys, zipfile

repo, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
src = os.path.join(repo, ".cache/archives/game/card/soliton.x86_64-aros-v11.zip")
idx = json.load(open(os.path.join(repo, "tests/fixtures/index-v03.json")))
entry = [p for p in idx["packages"] if p["id"] == "soliton"][0]

r1 = open(src, "rb").read()
assert hashlib.sha256(r1).hexdigest() == entry["sha256"], "cached soliton is not the index's"

# revision 2: the guide gains a line, a NEWS file appears; everything else byte-identical
r2p = os.path.join(out, "soliton-r2.zip")
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(r2p, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Soliton/Soliton.guide":
            data += b"\n@{b}Revision 2 of the AROS package.@{ub}\n"
        zout.writestr(item, data)
    zout.writestr("Soliton/NEWS", b"revision 2: packaging only, the program is unchanged\n")
r2 = open(r2p, "rb").read()

e2 = dict(entry); e2["revision"] = 2; e2["size"] = len(r2); e2["sha256"] = hashlib.sha256(r2).hexdigest()
e2["url"] = "https://example.invalid/soliton-r2.zip"
json.dump({"schema": 1, "source": "test", "packages": [entry]}, open(os.path.join(out, "idx-sol1.json"), "w"), indent=1)
json.dump({"schema": 1, "source": "test", "packages": [e2]},    open(os.path.join(out, "idx-sol2.json"), "w"), indent=1)

def sha_member(blob, name):
    with zipfile.ZipFile(__import__("io").BytesIO(blob)) as z: return hashlib.sha256(z.read(name)).hexdigest()
g1 = sha_member(r1, "Soliton/Soliton.guide"); g2 = sha_member(r2, "Soliton/Soliton.guide")
news = hashlib.sha256(b"revision 2: packaging only, the program is unchanged\n").hexdigest()
prog = sha_member(r1, "Soliton/Soliton")

ROOT = "SYS:PkgSol"; A = f"RAM:apkg --root {ROOT} --machine onetest"
def run(rid, expect, cmd, idx=None):
    i = f" --index RAM:{idx}" if idx else ""
    return f"{A}{i} --run-id {rid} --expect {expect} --report RAM:srep/{rid}.json {cmd} >>RAM:sol-console.txt"
def ah(path, h): return f"{A} assert-hash {ROOT}/{path} {h} >>RAM:sol-console.txt"
def start(label):
    return f'''CD {ROOT}/soliton
Run >NIL: <NIL: Soliton
Wait 12
Echo >>RAM:sol-console.txt "{label}: soliton running? (a number means yes):"
Status >>RAM:sol-console.txt COMMAND=Soliton
Status >NIL: COMMAND=Soliton
If NOT WARN
  Break >NIL: `Status COMMAND=Soliton` C
EndIf
Wait 4
CD SYS:'''

L = f"""; A real program through an upgrade and a rollback: the published soliton
; archive as revision 1, a repackaged copy as revision 2. No network: both
; archives are placed in the cache with the hashes the indexes expect.
;     Execute PAYLOAD:S/SOLUP

FAILAT 1000
Delete RAM:sol-console.txt QUIET
Copy PAYLOAD:APKG RAM:apkg
Copy PAYLOAD:PUTFILE RAM:putfile
Copy PAYLOAD:I/IDXS1 RAM:idx-sol1.json
Copy PAYLOAD:I/IDXS2 RAM:idx-sol2.json
Protect RAM:apkg +e
Protect RAM:putfile +e
Delete {ROOT} ALL FORCE QUIET
Delete RAM:srep ALL FORCE QUIET
MakeDir RAM:srep
MakeDir {ROOT}
MakeDir {ROOT}/cache
Copy PAYLOAD:A/SOLR1 {ROOT}/cache/soliton.zip
Copy PAYLOAD:A/SOLR2 {ROOT}/cache/soliton.new.zip

Echo >>RAM:sol-console.txt "== install the published revision =="
{run("s-install", 0, "install soliton", "idx-sol1.json")}
{ah("soliton/Soliton.guide", g1)}
{start("before upgrade")}
Echo >>RAM:sol-console.txt "*N== upgrade to revision 2: the guide replaced, NEWS added, the program untouched =="
{run("s-upgrade", 0, "upgrade soliton", "idx-sol2.json")}
{ah("soliton/Soliton.guide", g2)}
{ah("soliton/NEWS", news)}
{ah("soliton/Soliton", prog)}
{A} info soliton >>RAM:sol-console.txt
{start("after upgrade")}
Echo >>RAM:sol-console.txt "*N== rollback: the guide back, NEWS gone, the program untouched =="
{run("s-rollback", 0, "rollback soliton")}
{ah("soliton/Soliton.guide", g1)}
{ah("soliton/Soliton", prog)}
If EXISTS {ROOT}/soliton/NEWS
  Echo >>RAM:sol-console.txt "FAIL s-rollback: NEWS still there"
Else
  Echo >>RAM:sol-console.txt "PASS s-rollback: NEWS gone"
EndIf
{start("after rollback")}
Echo >>RAM:sol-console.txt "*N== remove =="
{run("s-remove", 0, "remove soliton")}
List {ROOT} >>RAM:sol-console.txt
Echo >>RAM:sol-console.txt "== end =="
Join RAM:srep/#?.json AS RAM:sol-reports.txt
RAM:putfile RAM:sol-console.txt
RAM:putfile RAM:sol-reports.txt
"""
open(os.path.join(out, "SOLUP"), "w").write(L)
print("real fixture in", out, "r2 sha", e2["sha256"][:16])
