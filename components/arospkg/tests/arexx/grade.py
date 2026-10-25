#!/usr/bin/env python3
"""Grade an ARexx acceptance run from what the guest wrote to RESULTS:.

    grade.py RUNID RESULTSDIR STAGEDIR.sha256 [REPORT.md]
    grade.py --examples RUNID RESULTSDIR STAGEDIR.sha256 [REPORT.md]

The guest already decided PASS or FAIL for every check it could make
(rxtest.rexx). This adds what only the host can see across files: that every
phase ran and reported, that every answer came from the binary that was
staged, and what the window's own event logs say happened -- a second
witness, written by PkgManager rather than by the script. Exit 0 only when
everything holds.
"""
import os, re, sys

examples = sys.argv[1] == "--examples"
if examples: sys.argv.pop(1)
run, res, shafile = sys.argv[1], sys.argv[2], sys.argv[3]
report = sys.argv[4] if len(sys.argv) > 4 else None

staged = {}
for line in open(shafile):
    h, name = line.split()
    staged[name] = h
want_sha = staged["PkgManager"]

def grade_examples():
    """Each example's own output, as a user would read it, and the window's log."""
    want = [
        ("isinstalled-before", ["soliton is not installed"]),
        ("install", ["installing soliton", "soliton is installed"]),
        ("isinstalled-after", ["soliton is installed, version 2.2"]),
        ("isinstalled-unknown", ["cannot tell: NOTFOUND"]),
        ("show", ["PkgManager shows cls"]),
        ("watch-cancel", ["running download", "cancel requested", "outcome: cancelled CANCELLED"]),
        ("install-watch", ["running extract", "outcome: done"]),
        ("upgrade", ["confirm or cancel the upgrade of soliton in PkgManager", "soliton upgraded"]),
    ]
    rows, problems = [], []
    for name, needles in want:
        p = os.path.join(res, f"{run}-ex-{name}.txt")
        t = open(p, encoding="latin-1").read() if os.path.exists(p) else None
        missing = [n for n in needles if t is None or n not in t]
        rows.append((name, "ok" if not missing else "missing: " + "; ".join(missing), (t or "").strip().replace("\n", " / ")[:200]))
        problems += [f"ex-{name}: '{n}' not in the output" for n in missing]
    log = open(os.path.join(res, f"{run}-ex-pm1.log"), encoding="latin-1").read() if os.path.exists(os.path.join(res, f"{run}-ex-pm1.log")) else ""
    for needle, what in ((f"sha256 {want_sha}", "the log names the staged binary"),
                         ("upgrade soliton confirmed\n", "the upgrade was confirmed in the window")):
        if needle not in log: problems.append(f"ex-pm1: {what}")
    lines = [f"# ARexx examples run {run}", "", f"PkgManager `{want_sha}`.", "",
             "| example | verdict | output |", "|---|---|---|"]
    lines += [f"| {a} | {b} | `{c}` |" for a, b, c in rows]
    lines += [""] + ([f"- {p}" for p in problems] or ["Everything holds."])
    text = "\n".join(lines) + "\n"
    print(text)
    if report: open(report, "w").write(text)
    sys.exit(1 if problems else 0)

if examples: grade_examples()

PHASES = ["env", "parse", "stress", "queries0", "show", "installstart", "installcheck",
          "remove", "rapid", "spin", "select", "guibusy", "clilock", "upgradestart", "upgradecheck",
          "rollbackstart", "rollbackcheck", "cancel", "quitstart", "gone", "collide",
          "closestart", "gone2", "norexx", "nomem", "nomem2", "update"]

rows, problems = [], []

def read(name):
    p = os.path.join(res, name)
    return open(p, encoding="latin-1").read() if os.path.exists(p) else None

def note(ok, what):
    if not ok: problems.append(what)
    return ok

total_pass = total_fail = 0
for ph in PHASES:
    t = read(f"{run}-{ph}.txt")
    if t is None:
        note(False, f"{ph}: no result file"); rows.append((ph, "-", "-", "missing")); continue
    m = re.search(rf"^SUMMARY {ph} pass (\d+) fail (\d+)$", t, re.M)
    fails = re.findall(r"^FAIL (.*)$", t, re.M)
    shas = set(re.findall(r"^version PkgManager \S+ ARexx \d+ sha256 ([0-9a-f]{64})$", t, re.M))
    if not m:
        note(False, f"{ph}: no SUMMARY (the script stopped early)")
        rows.append((ph, len(re.findall(r'^PASS', t, re.M)), len(fails), "incomplete")); continue
    p, f = int(m.group(1)), int(m.group(2))
    total_pass += p; total_fail += f
    for x in fails: note(False, f"{ph}: FAIL {x}")
    if ph not in ("gone", "gone2", "norexx", "nomem", "nomem2"):
        note(shas == {want_sha}, f"{ph}: answered by {shas or 'nothing'}, staged {want_sha}")
    rows.append((ph, p, f, "ok" if f == 0 else "failed"))

# The WAITs parked by Run, one file per job: across the upgrade's
# confirmation, the QUIT and the close gadget. All three must be there --
# a WAIT that never came back writes nothing -- and each must have ended
# with a final state, answered by the staged binary.
waitfiles = sorted(fn for fn in os.listdir(res) if fn.startswith(f"{run}-waitjob-"))
note(len(waitfiles) == 3, f"waitjob: {len(waitfiles)} result files, 3 expected")
for fn in waitfiles:
    t = read(fn)
    shas = set(re.findall(r"^version PkgManager \S+ ARexx \d+ sha256 ([0-9a-f]{64})$", t, re.M))
    ok = re.search(r"^SUMMARY waitjob pass \d+ fail 0$", t, re.M) is not None and "PASS wait-returned-final" in t
    note(ok, f"{fn}: no final state reported")
    note(shas == {want_sha}, f"{fn}: answered by {shas or 'nothing'}")
    rows.append((fn[len(run) + 1:-4], len(re.findall(r'^PASS', t, re.M)), len(re.findall(r'^FAIL', t, re.M)), "ok" if ok else "failed"))

# the window's own record
host_before = len(problems)
pm1, pm2, pm3, pm4, pm5, pm6, pm7 = (read(f"{run}-{x}.log") or "" for x in ("pm1", "pm2", "pm3", "pm4", "pm5", "pm6", "pm7"))
logchecks = [
    (pm1, f"sha256 {want_sha}", "pm1: the log names the staged binary"),
    (pm1, ": ARexx port PKGMANAGER", "pm1: the port was opened"),
    (pm1, "refill (arexx show)", "pm1: SHOW refilled the list"),
    (pm1, "upgrade soliton declined (ARexx CANCEL)", "pm1: CANCEL declined the first upgrade"),
    (pm1, "upgrade soliton confirmed\n", "pm1: the second upgrade was confirmed in the window"),
    (pm1, "rollback soliton confirmed\n", "pm1: the rollback was confirmed in the window"),
    (pm1, "from window", "pm1: a job started from the window"),
    (pm1, "cancel asked by ARexx for job", "pm1: ARexx cancel reached the worker"),
    (pm1, "arexx< rc=0 len=7 closing", "pm1: QUIT answered"),
    (pm1, "\nquit", "pm1: the first window quit"),
    (pm3, "no ARexx port: PKGMANAGER is already in use", "pm3: the second window went without a port"),
    (pm2, "\nquit", "pm2: that window quit"),
    (pm4, "no ARexx port: nosuch.library cannot be opened", "pm4: no rexxsyslib, no port"),
    (pm4, "remove pressed for cls", "pm4: the window still works"),
    (pm4, "done remove cls status=0", "pm4: and removed cls"),
    (pm5, "done update  status=0", "pm5: UPDATE fetched the catalogue"),
    (pm6, f"sha256 {want_sha}", "pm6: the no-memory window is the staged binary"),
    (pm6, "job 1 install cls failed CANNOTSTART", "pm6: no final message, no job"),
    (pm6, "NOMEM no memory for the answer", "pm6: a long answer failed as NOMEM"),
    (pm7, f"sha256 {want_sha}", "pm7: the prepare-nomem window is the staged binary"),
    (pm7, "NOMEM no memory for the answer; nothing was started", "pm7: INSTALL refused before starting"),
]
host_total = len(logchecks) + 4
for text, needle, what in logchecks:
    note(needle in text, what)
note("--yes" not in pm1, "pm1: no automatic confirmation")
note("update pressed" not in pm1, "pm1: Update index did nothing while a plan waited")
# the close gadget: it must reach the window while the install runs (not
# during the removal before it), and the install must end before the window
m = re.search(r"\njob \d+ install cls from arexx\n(.*?)\ndone install cls", pm2, re.S)
note(m is not None and "close requested during" in m.group(1), "pm2: the close gadget during an install")
note("synchronised with the install: yes" in (read(f"{run}-host-closestart.txt") or ""),
     "driver: the close click waited for the install to show")
host_total += 2
# the close gadget: the install must end before the window does
if "close requested during" in pm2:
    tail = pm2.split("close requested during", 1)[1]
    note(re.search(r"done install cls status=0", tail) is not None and tail.find("done install cls") < tail.find("\nquit"),
         "pm2: the install finished before the window quit")
final = read(f"{run}-final-apkg-list.txt") or ""
note("message for job" not in pm1, "pm1: no message was ever for another job")
note("job 1 " not in pm7, "pm7: no job was ever created")
# the window acted on a search and on clearing it while the polled install ran
spin_ok = any(re.search(r'^refill \(search\) term="cls"', mm.group(2), re.M) and re.search(r'^refill \(search\) term=""', mm.group(2), re.M)
              for mm in re.finditer(r"arexx> INSTALL cls\njob (\d+) install cls from arexx\n(.*?)job \1 install cls done", pm1, re.S))
spin_ok = spin_ok and re.search(r"^arexx> \(the line above \d{3,} more times\)$", pm1, re.M) is not None
note(spin_ok, "pm1: a search typed in the window was handled while a script polled without pause")
host_total += 3
note(re.search(r"^soliton\b", final, re.M) is not None and re.search(r"^cls\b", final, re.M) is None,
     "final: apkg list shows soliton and not cls")

lines = [f"# ARexx acceptance run {run}", "",
         f"PkgManager `{want_sha}`, apkg `{staged.get('apkg', '?')}`.", "",
         "| phase | pass | fail | |", "|---|---:|---:|---|"]
lines += [f"| {a} | {b} | {c} | {d} |" for a, b, c, d in rows]
lines += ["", f"Guest checks: {total_pass} passed, {total_fail} failed. "
              f"Host checks on the event logs and the final state: {host_total - (len(problems) - host_before)} of {host_total} hold.", ""]
if problems:
    lines += ["Problems:", ""] + [f"- {p}" for p in problems]
else:
    lines += ["Everything holds."]
text = "\n".join(lines) + "\n"
print(text)
if report: open(report, "w").write(text)
sys.exit(1 if problems else 0)
