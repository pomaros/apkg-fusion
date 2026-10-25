#!/usr/bin/env python3
"""Grade a run of tests/rc1net from what the guest wrote to RESULTS:.

    grade.py RUNID RESULTSDIR STAGEDIR.sha256 [REPORT.md]

Every phase is PASS (its file ends with SUMMARY and no FAIL), FAIL, or
NOT RUN (no file, or no SUMMARY: the script never got there or stopped).
Each answering phase must come from the binary meant for it: the published
PkgManager for the network phases, PkgManager-entropyfail for the entropy
phases. The windows' own logs must name the same binaries. Exit 0 only when
every phase passes.
"""
import os, re, sys

run, res, shafile = sys.argv[1], sys.argv[2], sys.argv[3]
report = sys.argv[4] if len(sys.argv) > 4 else None
staged = {l.split()[1]: l.split()[0] for l in open(shafile) if l.strip()}
rel, ef = staged["PkgManager"], staged["PkgManager-entropyfail"]

PHASES = [  # phase, binary that must answer (None: no port by design), what it shows
    ("update", rel, "Update from the window's worker: the real catalogue over HTTPS"),
    ("archives", rel, "install gmore from AROS Archives on an empty cache"),
    ("github", rel, "install micropolis from GitHub Releases on an empty cache"),
    ("quitnet", rel, "close the window"),
    ("gonenet", None, "window and lock gone"),
    ("tls", rel, "expired certificate refused; nothing installed or left behind"),
    ("recover", rel, "the next install after the refusal succeeds"),
    ("quittls", rel, "close the window"),
    ("gonetls", None, "window and lock gone"),
    ("entfail", ef, "forced entropy failure: Update refused, nothing written, window responsive"),
    ("quitent", ef, "close the window"),
    ("goneent", None, "window and lock gone"),
]
VER = re.compile(r"^version PkgManager \S+ ARexx \d+ sha256 ([0-9a-f]{64})$", re.M)

def read(name):
    p = os.path.join(res, name)
    return open(p, encoding="latin-1").read() if os.path.exists(p) else None

rows, problems = [], []
for ph, want, what in PHASES:
    t = read(f"{run}-{ph}.txt")
    m = t and re.search(rf"^SUMMARY {ph} pass (\d+) fail (\d+)$", t, re.M)
    if not m:
        rows.append((ph, "NOT RUN", "-", "-", what))
        problems.append(f"{ph}: NOT RUN ({'no result file' if t is None else 'no SUMMARY, the script stopped'})")
        continue
    p, f = int(m.group(1)), int(m.group(2))
    fails = re.findall(r"^FAIL (.*)$", t, re.M)
    problems += [f"{ph}: FAIL {x}" for x in fails]
    if want:
        shas = set(VER.findall(t))
        if shas != {want}:
            problems.append(f"{ph}: answered by {shas or 'nothing'}, expected {want}")
            f += 1
    rows.append((ph, "PASS" if f == 0 else "FAIL", p, f, what))

for log, want in (("net", rel), ("tls", rel), ("ent", ef)):
    t = read(f"{run}-pm-{log}.log")
    if t is None: problems.append(f"pm-{log}.log: not collected")
    elif f"sha256 {want}" not in t: problems.append(f"pm-{log}.log: does not name {want}")

lines = [f"# rc1 network acceptance, run {run}", "",
         f"- published PkgManager `{rel}`",
         f"- fault-injection PkgManager-entropyfail `{ef}` (its results are not the release binary's)",
         f"- apkg `{staged['apkg']}`", "",
         "| phase | verdict | pass | fail | what it shows |", "|---|---|---|---|---|"]
lines += [f"| {a} | {b} | {c} | {d} | {e} |" for a, b, c, d, e in rows]
lines += ["", "Problems:" if problems else "Every phase passed."] + [f"- {x}" for x in problems]
text = "\n".join(lines) + "\n"
print(text)
if report: open(report, "w").write(text)
sys.exit(1 if problems else 0)
