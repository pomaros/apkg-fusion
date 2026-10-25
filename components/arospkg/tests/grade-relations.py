#!/usr/bin/env python3
"""Grade a relations.script run collected from RESULTS:.

    grade-relations.py RESULTSDIR TAG"""
import json, sys
from pathlib import Path

d, tag = Path(sys.argv[1]), sys.argv[2]
fails = 0


def f(name):
    hits = sorted(d.glob(f"{tag}-{name}*"))
    return hits[0].read_text(errors="replace") if hits else ""


def rc(t):
    lines = [l for l in t.splitlines() if l.startswith("rc ")]
    return int(lines[-1][3:]) if lines else None


def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += not ok


t = f("02"); check(rc(t) == 5 and "install (needed) limpet" in t and "Nothing was changed" in t, "a plan with a dependency waits for --yes")
t = f("03"); check(rc(t) == 0 and "install (needed) limpet" in t and "dry run: nothing was changed" in t, "dry run shows the plan and the files")
t = f("04"); check(rc(t) == 0 and "Installed (needed) limpet" in t and "Installed hardcode" in t, "dependency installed first, then the package")
check('"reason":"dependency"' in f("05"), "the dependency is recorded as such")
t = f("06"); check(rc(t) == 10 and "hardcode" in t and "need" in t, "removing what another package needs is refused")
t = f("07"); check("recommends:   limpet >= 0.4.1" in t and "replaces:     hardcode: installed" in t, "show: recommendation and replacement")
js = json.loads(f("08-bishop-plan.json") or "{}")
check([s["action"] for s in js.get("steps", [])] == ["install", "remove-replaced"], "JSON plan: install, then remove the replaced one")
t = f("09"); check(rc(t) == 5, "the replacement waits for --yes")
t = f("10"); check(rc(t) == 10 and "2. remove (replaced) hardcode: not done" in t, "interrupted: says which steps ran")
t = f("11"); check("bishop.json" in t and "hardcode.json" in t and '"old":["hardcode"]' in t, "between: both installed, the replacement recorded")
t = f("12"); check("finished an interrupted replacement: hardcode by bishop" in t, "recovery finishes the replacement and says so")
t = f("13"); check("hardcode.json" not in t and "my-own-file.txt" in t and "ReadMe.txt" in t, "old entry gone, the user's files kept")
t = f("16"); check(rc(t) == 0, "Limpet, only recommended now, may be removed")
t = f("17"); check("not installed; the catalogue has 0.5.0" in t, "show: recommendation absent, and where to get it")
t = f("20"); check(rc(t) == 0 and "Installed (needed) rellib" in t, "transitive: installed in order")
for n, word, what in (("21", "relapp", "removal guard"), ("22", "relenemy says it conflicts", "conflict declared by the new package"),
                      ("24", "the installed relfoe says", "conflict declared by the installed package"),
                      ("25", "Upgrade it first", "installed dependency too old"), ("26", "x86_64/v1", "dependency only for another ABI"),
                      ("27", "more than one build", "ambiguous"), ("28", "a cycle", "cycle"), ("29", "does not list", "missing from the catalogue")):
    t = f(n); check(rc(t) == 10 and word in t, what)
t = f("30"); check(rc(t) == 0 and '"reason":"explicit"' in t, "installing a dependency by name records it as yours")
t = f("31"); check(rc(t) == 10 and "install relbroken: not done" in t, "a failed step: the rest is not done, and said")
t = f("32"); check("relbroken" not in t and "relbad" not in t, "nothing of the failed plan is left installed")
t = f("33"); check("no unresolved transactions" in t, "doctor: nothing unresolved")
check(bool(f("99")), "script completed")
print(f"\n{fails} failed" if fails else "\nall passed")
sys.exit(1 if fails else 0)
