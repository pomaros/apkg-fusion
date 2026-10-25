#!/usr/bin/env python3
"""Grade a tests/netbreak run: every phase present, with SUMMARY and no FAIL.

    grade.py RUNID RESULTSDIR STAGEDIR.sha256

The window's answers must come from the staged PkgManager (VERSION's hash)
and the CLI's from the staged apkg (--version prints its hash). Exit 0 only
when all of it holds.
"""
import os, re, sys
run, res, shafile = sys.argv[1:4]
staged = {l.split()[1]: l.split()[0] for l in open(shafile) if l.strip()}
problems = []
for ph in ("cli", "gui", "gone"):
    p = os.path.join(res, f"{run}-{ph}.txt")
    if not os.path.exists(p):
        problems.append(f"{ph}: NOT RUN (no result file)"); print(f"{ph:5} NOT RUN"); continue
    t = open(p, encoding="latin-1").read()
    m = re.search(rf"^SUMMARY {ph} pass (\d+) fail (\d+)$", t, re.M)
    if not m:
        problems.append(f"{ph}: NOT RUN (no SUMMARY)"); print(f"{ph:5} NOT RUN"); continue
    problems += [f"{ph}: FAIL {x}" for x in re.findall(r"^FAIL (.*)$", t, re.M)]
    print(f"{ph:5} {'PASS' if m.group(2) == '0' else 'FAIL'}  pass {m.group(1)} fail {m.group(2)}")
cli = os.path.join(res, f"{run}-cli-version.txt")
if not (os.path.exists(cli) and staged["apkg"] in open(cli, encoding="latin-1").read()):
    problems.append("cli: --version does not name the staged apkg")
gui = os.path.join(res, f"{run}-gui.txt")
if os.path.exists(gui) and staged["PkgManager"] not in open(gui, encoding="latin-1").read():
    problems.append("gui: VERSION does not name the staged PkgManager")
for p in problems: print("-", p)
sys.exit(1 if problems else 0)
