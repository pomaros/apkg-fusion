#!/usr/bin/env python3
"""Grade a guest run from its evidence FILES, never from a screen.

    grade.py <script> <reports.txt> [console.txt] [--main SHA] [--binary RUNID=SHA ...]

Every `--run-id X ... --expect N` in the script is an expectation; every report
in the joined reports file is an outcome. A run with an expectation and no
report is a FAILURE, not a skip: a test that produced nothing did not pass.
The console, when given, is searched for the PASS/FAIL lines that presence and
hash checks print. Exit status 0 only if everything expected happened.

--main names the binary every report must come from -- the one in the
release archive under test. --binary pins one run to another binary, for a
suite that uses two on purpose (release-1g: the 2026-09-12 binary writes an
entry, this one removes it). Without either, all reports must share one
binary, whichever it is.
"""
import json, re, sys

args = sys.argv[1:]
main_sha, pins, pos = None, {}, []
while args:
    a = args.pop(0)
    if a == "--main": main_sha = args.pop(0)
    elif a == "--binary":
        rid, sha = args.pop(0).split("=", 1); pins[rid] = sha
    else: pos.append(a)
script, reports = pos[0], pos[1]
console = open(pos[2], encoding="latin-1").read() if len(pos) > 2 else None

want = {}
for line in open(script):
    if line.lstrip().startswith(";"):
        continue
    r = re.search(r"--run-id (\S+)", line)
    if r:
        e = re.search(r"--expect (\d+)", line)
        want[r.group(1)] = int(e.group(1)) if e else None

txt = open(reports, encoding="latin-1").read()
reps = [json.loads(m) for m in re.findall(r'\{ "run_id".*?\}\n', txt, re.S)]
got = {r["run_id"]: r for r in reps}
bins = sorted({r["binary_sha256"] for r in reps})

bad = 0
print(f"reports {len(reps)}, expectations {len(want)}, binaries {bins}")
for rid, w in want.items():
    r = got.get(rid)
    if r is None:
        print(f"  FAIL {rid}: no report"); bad += 1
    elif w is not None and r["status"] != w:
        print(f"  FAIL {rid}: status {r['status']} ({r['status_text']}), expected {w}"); bad += 1
for rid, sha in pins.items():
    r = got.get(rid)
    if r is not None and r["binary_sha256"] != sha:
        print(f"  FAIL {rid}: from {r['binary_sha256'][:8]}, pinned to {sha[:8]}"); bad += 1
rest = sorted({r["binary_sha256"] for r in reps if r["run_id"] not in pins})
if main_sha is not None:
    if rest and rest != [main_sha]:
        print(f"  FAIL: reports come from {[b[:8] for b in rest]}, not only the binary under test {main_sha[:8]}"); bad += 1
elif len(rest) > 1:
    print("  FAIL: reports come from more than one binary"); bad += 1

if console is not None:
    fails = [l for l in console.splitlines() if "FAIL" in l]
    passes = [l for l in console.splitlines() if l.startswith("PASS")]
    print(f"console: {len(passes)} PASS lines, {len(fails)} lines containing FAIL")
    for l in fails:
        print(f"  {l}")
    bad += len(fails)

print("RESULT:", "all expectations met" if not bad else f"{bad} failure(s)")
sys.exit(1 if bad else 0)
