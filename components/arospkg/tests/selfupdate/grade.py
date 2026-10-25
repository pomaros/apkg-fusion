#!/usr/bin/env python3
"""Grade a self-update run from the result files the guest scripts wrote.

    grade.py RESULTSDIR RUNID

Every "PASS"/"FAIL" line counts, every "rc=N want=M" must have N == M, and
the messages that say what happened must be there: a refusal that changes
nothing is only a pass when it also says why. Exit 0 when everything holds.
"""
import os, re, sys

d, run = sys.argv[1], sys.argv[2]
# file -> phrases that must appear in it
WANT = {
    "s1": ["Recorded: AROS:C/apkg belongs to that installation", "Up to date",
           "none: apkg on its own, without PkgManager"],
    "a1": ["none: apkg on its own"],
    "e": ["does not match the release's checksum", "ended after",
          "no archive for this CPU", "protected from deletion", "cannot write the new file",
          "The previous version is back", "operation is running", "the refresh finished undisturbed"],
    "r": ["finished: the new version is in place", "undone: the previous version is back"],
    "m1": ["is replaced as an upgrade", "0.7.0-sutest2"],
    "m2": ["is replaced as an upgrade"],
    "mr": ["stopped inside the package transaction", "undone: the previous version is back"],
    "j": ["an earlier update of arospkg did not finish", "JrnA", "finished: the new version is in place"],
    "s2": ["Updated apkg to 0.7.0-sutest2", "Updated arospkg to 0.7.0-sutest2"],
    "k": ["cannot record which installation", "none: apkg on its own"],
    "mv": ["changed since the update was planned", "0.8.0"],
    "a2": ["not recorded which installation", "SUT/Root With Space/arospkg", "Old Apps", "--apkg-only",
           "used on its own, as recorded", "PkgManager is running from this installation",
           "Updated arospkg to 0.7.1-sutest3"],
}
ok = bad = 0
for name, phrases in WANT.items():
    p = os.path.join(d, "%s-%s.txt" % (run, name))
    if not os.path.exists(p):
        print("MISSING", p); bad += 1; continue
    text = open(p, errors="replace").read()
    for line in text.splitlines():
        if line.startswith("PASS"): ok += 1
        elif line.startswith("FAIL"): print(name, line); bad += 1
        m = re.match(r"rc=(\d+) want=(\d+)", line)
        if m:
            if m.group(1) == m.group(2): ok += 1
            else: print(name, "wrong return code:", line); bad += 1
    flat = " ".join(text.split())
    for ph in phrases:
        if ph in flat: ok += 1
        else: print(name, "missing:", ph); bad += 1
print("%d checks passed, %d failed" % (ok, bad))
sys.exit(1 if bad else 0)
