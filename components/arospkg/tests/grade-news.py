#!/usr/bin/env python3
"""Grade a news.script run collected from RESULTS:.

    grade-news.py RESULTSDIR TAG EXPECT_JSON

Prints PASS/FAIL per check and exits 1 on any failure."""
import json, sys
from pathlib import Path

d, tag, expect = Path(sys.argv[1]), sys.argv[2], json.loads(Path(sys.argv[3]).read_text())
fails = 0


def f(name):
    hits = sorted(d.glob(f"{tag}-{name}*"))
    return hits[0].read_text(errors="replace") if hits else ""


def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += not ok


def rc(text):
    lines = [l for l in text.splitlines() if l.startswith("rc ")]
    return int(lines[-1][3:]) if lines else None


t = f("02-first-update")
check(rc(t) == 0 and "first catalogue on this machine" in t and "Recently added to the catalogue:" in t,
      "first update: says it is the first catalogue and lists recent additions")
check("New application" not in t, "first update: nothing called new")
t = f("03-update-again")
check(rc(t) == 0 and "Nothing new for this system" in t, "second update without changes: nothing new")
t = f("05-update-b")
check(rc(t) == 0 and "New applications (3)" in t and "xrick (back)" in t, "update over an older catalogue: 3 new, xrick back")
check("New build for this system (1): folio" in t, "folio: a new build for this system")
check("Updated in the catalogue (2)" in t and "zgloom" in t and "scopagame" in t, "zgloom and scopagame: updated")
check("Full list: apkg --root" in t and "recent --since-last-update" in t, "summary points at the full list")
check("Compared with the catalogue fetched 2026-10-01 12:00" in f("06-last"), "compared with the catalogue of the recorded refresh")
check(rc(f("05a-seeded-last")) == 0, "a seeded record of 2026-10-01 is read")
js = json.loads(f("07-last.json") or "{}")
kinds = {}
for it in js.get("items", []):
    kinds.setdefault(it["kind"], []).append(it["id"])
check(sorted(kinds.get("new-application", [])) == expect["new-application"], f"JSON new applications {kinds.get('new-application')}")
check(sorted(kinds.get("new-build", [])) == expect["new-build"], f"JSON new builds {kinds.get('new-build')}")
check(sorted(kinds.get("updated", [])) == expect["updated"], f"JSON updated {kinds.get('updated')}")
check(sorted(i["id"] for i in js.get("items", []) if i["returned"]) == expect["returned"], "JSON returned")
check(js.get("recorded") is True and js.get("current") is True and js.get("first_catalogue") is False,
      "JSON: recorded, current, not first")
check(rc(f("07-last-rc")) == 0, "redirected JSON: rc 0")
t = f("08-recent")
check(rc(t) == 0 and "Recently added, for x86_64/v11" in t and "new builds of older applications too" in t.lower(),
      "recent: heading and --builds hint")
t9 = f("09-recent-all")
check(rc(t9) == 0 and t9.count("  added") > 10 and "Recently added" in t9, "recent --builds --limit 0: every dated application, labelled added")
check(rc(f("10-recent-days")) == 0, "recent --days 30: rc 0")
js = json.loads(f("11-since.json") or "{}")
check(js.get("mode") == "recent" and all((i["first_added"] if i["kind"] == "new-build" else i["app_first_added"]) >= "2026-10-01"
                                          for i in js.get("items", [])), "recent --since --json: only dates on or after")
t = f("12-usage")
check([l for l in t.splitlines() if l.startswith("rc ")] == ["rc 5", "rc 5"], "bad date and --days with --since: rc 5")
check(rc(f("14-cancel")) != 0 and "cancel" in f("14-cancel").lower(), "cancelled update reports it")
check(f("13-news-before") == f("15-news-after-cancel") and f("13-index-before") == f("15-index-after-cancel"),
      "cancel: catalogue and record unchanged")
check(rc(f("16-fail")) == 10, "failed update: rc 10")
check(f("13-news-before") == f("17-news-after-fail") and f("13-index-before") == f("17-index-after-fail"),
      "failure: catalogue and record unchanged")
check("New applications (3)" not in f("18-last-kept") and "xrick" in f("18-last-kept"),
      "after cancel and failure, the last update's list is still there")
t = f("19-show-xrick")
check("status:" in t and "runtime tests:" in t and "added:" in t and "catalogue:" in t, "show: status, tests, added, catalogue")
t = f("22-nocat")
check([l for l in t.splitlines() if l.startswith("rc ")] == ["rc 10", "rc 10"], "no catalogue: recent rc 10 twice")
t = f("23-update-again-b")
check(rc(t) == 0 and "Nothing new for this system" in t, "update again on B: nothing new")
t = f("24-other-address")
check(rc(t) == 0 and "another address" in t, "another address: not compared")
check(bool(f("99-done")), "script completed")
print(f"\n{fails} failed" if fails else "\nall passed")
sys.exit(1 if fails else 0)
