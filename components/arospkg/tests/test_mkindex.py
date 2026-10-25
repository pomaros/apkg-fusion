#!/usr/bin/env python3
"""tools/mkindex.py: what it refuses, and what it does when it refuses.

Each case changes one field of a manifest that passes, and names the reason
that must appear. The publication cases run the tool itself and check its
exit status and that a previous index survives a failed run.
"""
import json, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "tools"))
import mkindex  # noqa: E402

GOOD = dict(status="approved", id="demo", version="1.0", arch="x86_64", abi="v11",
            url="https://example.invalid/demo.lha", size=10, sha256="a" * 64,
            kind="app", depends=[], depends_checked=True, subdir="Demo",
            icon="Demo.info", requires_system=[{"type": "library", "id": "crt.library"}])
fails = []


def expect(name, change, want):
    m = {**GOOD, **change}
    for k, v in change.items():
        if v is None: del m[k]
    got = mkindex.check(m)
    if want is None:
        if got: fails.append(f"{name}: expected to pass, got {got}")
    elif not any(want in p for p in got):
        fails.append(f"{name}: expected a reason with {want!r}, got {got}")


expect("the base passes", {}, None)
expect("abi missing", {"abi": None}, "abi None is not one the client knows")
expect("abi unknown", {"abi": "v9"}, "abi 'v9' is not one the client knows")
expect("abi v0 on i386", {"abi": "v0", "arch": "i386"}, None)
expect("arch unknown", {"arch": "m68k"}, "arch 'm68k' is not one we build for")
expect("abi v1", {"abi": "v1"}, None)
expect("plain http", {"url": "http://example.invalid/demo.lha"}, "url is not https://")
expect("revision 0", {"revision": 0}, "revision 0 is not an integer")
expect("revision negative", {"revision": -1}, "revision -1 is not an integer")
expect("revision text", {"revision": "2"}, "revision '2' is not an integer")
expect("revision bool", {"revision": True}, "revision True is not an integer")
expect("revision 4", {"revision": 4}, None)
expect("size text", {"size": "10"}, "size '10' is not a positive integer")
expect("quote in version", {"version": '1.0 "beta"'}, "version '1.0 \"beta\"' has a character")
expect("backslash in subdir", {"subdir": "Demo\\x"}, "subdir")
expect("non-ASCII summary", {"summary": "Café"}, "summary 'Café' has a character")
expect("long url", {"url": "https://e.invalid/" + "a" * 500}, "url is 518 characters")
expect("long id", {"id": "a" * 64}, "id is 64 characters")
expect("long licence is a note", {"license": "x" * 80}, None)
expect("requirement not ASCII", {"requires_system": [{"type": "library", "id": "é.library"}]},
       "requires_system[0] id")
expect("min_version bool", {"requires_system": [{"type": "library", "id": "a.library", "min_version": True}]},
       "min_version True")

# [launch] (docs/rfc/0002): the same rules as the client's reader.
L = lambda **kw: {"launch": {"executable": "Demo", "mode": "workbench", **kw}}
expect("launch: workbench passes", L(), None)
expect("launch: shell with a directory passes", {"launch": {"executable": "bin/Tool", "mode": "shell", "working_directory": "Data Files"}}, None)
expect("launch: spaces pass", {"launch": {"executable": "My Tool", "mode": "shell"}}, None)
expect("launch: not a table", {"launch": "Demo"}, "launch: not a table")
expect("launch: no mode", {"launch": {"executable": "Demo"}}, "launch.mode")
expect("launch: unknown mode", L(mode="cli"), "launch.mode")
expect("launch: unknown key", L(arguments="-x"), "unknown key(s) arguments")
expect("launch: absolute", L(executable="/Demo"), "is absolute")
expect("launch: volume", L(executable="SYS:C/Dir"), "names a volume")
expect("launch: parent", L(executable="../Demo"), "leads out of the drawer")
expect("launch: dot component", L(executable="./Demo"), "'.' component")
expect("launch: the icon", L(executable="Demo.info"), "is not a program")
expect("launch: non-ASCII", L(executable="Démo"), "cannot carry")
expect("launch: workbench elsewhere", L(working_directory="Data"), "own drawer")
expect("launch: workbench in its own subdrawer", L(executable="bin/Demo", working_directory="bin"), None)

# The archive must hold what [launch] names; a workbench start, the icon.
names = {"Demo/", "Demo/Demo", "Demo/Demo.info", "Demo.info"}
if mkindex.check_members({**GOOD, **L()}, names):
    fails.append("launch members: a complete drawer was refused")
if not any("not in" in p for p in mkindex.check_members({**GOOD, **L(executable="Other")}, names)):
    fails.append("launch members: a missing program was accepted")
if not any("needs the program's icon" in p for p in mkindex.check_members({**GOOD, **L()}, names - {"Demo/Demo.info"})):
    fails.append("launch members: workbench without an icon was accepted")
if mkindex.check_members({**GOOD, "launch": {"executable": "Demo", "mode": "shell"}}, names - {"Demo/Demo.info"}):
    fails.append("launch members: shell mode needs no icon")
# An application.json in an archive must say what the manifest says.
good_app = json.dumps(mkindex.app_description({**GOOD, **L()}))
if mkindex.app_description_problems({**GOOD, **L()}, good_app, "a"):
    fails.append("application.json: the generated file disagrees with its own manifest")
if not mkindex.app_description_problems({**GOOD, **L(mode="shell")}, good_app, "a"):
    fails.append("application.json: a different mode was accepted")
if not mkindex.app_description_problems(GOOD, good_app, "a"):
    fails.append("application.json without [launch] in the manifest was accepted")

expect("notes: plain lines pass", {"post_install_notes": ["Needs the original game data.", "Copy it into the drawer."]}, None)
expect("notes: not a list", {"post_install_notes": "one line"}, "not a list of strings")
expect("notes: nine lines", {"post_install_notes": ["x"] * 9}, "9 lines; maximum is 8")
expect("notes: line too long", {"post_install_notes": ["a" * 160]}, "post_install_notes[0]: 160 characters; maximum is 159")
expect("notes: line of the limit", {"post_install_notes": ["a" * 159]}, None)
expect("notes: escape sequence", {"post_install_notes": ["\x1b[31mred"]}, "post_install_notes[0]: has a character")
expect("notes: quote", {"post_install_notes": ['say "hi"']}, "post_install_notes[0]: has a character")
expect("notes: empty line", {"post_install_notes": ["ok", " "]}, "post_install_notes[1]: empty")
expect("notes: non-ASCII", {"post_install_notes": ["Café"]}, "post_install_notes[0]: has a character")


def toml(m):
    lines = []
    for k, v in m.items():
        if k == "requires_system": continue
        if isinstance(v, bool): lines.append(f"{k} = {'true' if v else 'false'}")
        elif isinstance(v, (int, list)): lines.append(f"{k} = {v!r}".replace("'", '"'))
        else: lines.append(f'{k} = "{v}"')
    return "\n".join(lines) + "\n"


def run(manifests, old_ids=None, overrides=None):
    """Returns exit code, index-v2.json, stdout, index.json. With old_ids, an
    index.json listing those ids is there before; without, none is."""
    global last_old
    with tempfile.TemporaryDirectory() as d:
        d = Path(d); (d / "m").mkdir()
        for name, text in manifests.items():
            (d / "m" / name).write_text(text)
        if overrides:
            (d / "overrides").mkdir()
            for name, text in overrides.items():
                (d / "overrides" / name).write_text(text)
        v2, old = d / "index-v2.json", d / "index.json"
        v2.write_text("PREVIOUS\n")
        if old_ids is not None:
            old.write_text(json.dumps({"schema": 1, "packages": [
                {"id": i, "arch": "x86_64", "abi": "v11", "sha256": "0" * 64} for i in old_ids]}))
        if overrides:
            (d / "m").rename(d / "manifests")
        mdir = d / ("manifests" if overrides else "m")
        r = subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(mdir),
                            "--out-dir", str(d), "--cache", str(d / "nocache")], capture_output=True, text=True)
        last_old = old.read_text() if old.exists() else None
        return r.returncode, v2.read_text(), r.stdout


skel = toml({**GOOD, "id": "sk", "status": "skeleton", "sha256": ""})
code, text, _ = run({"demo.x86_64.toml": toml(GOOD), "sk.x86_64.toml": skel})
if code != 0 or text == "PREVIOUS\n":
    fails.append(f"a skeleton beside a good manifest: exit {code}, index {'kept' if text == 'PREVIOUS\n' else 'written'}")

bad = toml({**GOOD, "id": "bad", "url": "http://example.invalid/b.lha"})
code, text, out = run({"demo.x86_64.toml": toml(GOOD), "bad.x86_64.toml": bad})
if code == 0 or text != "PREVIOUS\n" or "NOT WRITTEN" not in out:
    fails.append(f"an approved manifest refused: exit {code}, previous index {'kept' if text == 'PREVIOUS\n' else 'replaced'}")

dep = toml({**GOOD, "id": "dep", "depends": ["nothere"]})
code, text, _ = run({"demo.x86_64.toml": toml(GOOD), "dep.x86_64.toml": dep})
if code == 0 or text != "PREVIOUS\n":
    fails.append(f"an unmet dependency: exit {code}")

many = {f"p{i:03}.x86_64.toml": toml({**GOOD, "id": f"p{i:03}"}) for i in range(260)}
# Without an index.json yet, both files take everything, so both must fit
# within the margin kept below what clients up to 0.3.2 read.
code, text, out = run(many)
if code == 0 or text != "PREVIOUS\n" or "clients up to 0.3.2 read" not in out:
    fails.append(f"a first index.json past the old clients' capacity: exit {code}")
# index.json keeps the one package it listed; the other 259 go to v2 only.
code, text, out = run(many, ["p000"])
old = json.loads(last_old or "{}").get("packages", [])
if code != 0 or text.count('"id"') != 260 or [p["id"] for p in old] != ["p000"]:
    fails.append(f"v2 past the old limit, index.json kept to its packages: exit {code}")
# ...with its metadata brought up to date, not frozen at the old hash.
elif old[0]["sha256"] != "a" * 64:
    fails.append("index.json kept an old sha256 instead of the manifest's")
# If the packages index.json already lists outgrow it, nothing is written
# and nothing is dropped by the tool.
code, text, out = run(many, [f"p{i:03}" for i in range(260)])
if code == 0 or text != "PREVIOUS\n" or "by hand" not in out:
    fails.append(f"index.json outgrown by its own packages: exit {code}")

# Two ABIs of one program for one CPU are two variants, both in index-v2.json;
# index.json, which listed only the v11 one, keeps only that.
v1 = toml({**GOOD, "abi": "v1", "url": "https://example.invalid/demo-v1.lha"})
code, text, out = run({"demo.x86_64.v11.toml": toml(GOOD), "demo.x86_64.v1.toml": v1}, ["demo"])
abis = sorted(p["abi"] for p in json.loads(text)["packages"]) if code == 0 else []
olds = [p["abi"] for p in json.loads(last_old)["packages"]] if code == 0 else []
if abis != ["v1", "v11"] or olds != ["v11"]:
    fails.append(f"two ABI variants: exit {code}, v2 {abis}, index.json {olds}: {out}")

# launch reaches index-v2.json, with the directory filled in, and never
# index.json, which carries only what clients up to 0.3.2 read.
code, text, out = run({"demo.x86_64.v11.toml": toml(GOOD) + '\n[launch]\nexecutable = "Demo"\nmode = "workbench"\n'}, ["demo"])
v2 = json.loads(text)["packages"][0] if code == 0 else {}
oldp = json.loads(last_old)["packages"][0] if code == 0 else {}
if v2.get("launch") != {"executable": "Demo", "mode": "workbench", "working_directory": "."}:
    fails.append(f"launch in index-v2.json: exit {code}, {v2.get('launch')}: {out}")
if set(oldp) - set(mkindex.OLD_CLIENT_FIELDS):
    fails.append(f"index.json carries fields old clients do not read: {sorted(set(oldp) - set(mkindex.OLD_CLIENT_FIELDS))}")

# The package manager itself is always in index.json, so a client without
# self-update can reach a current apkg, even when index.json never listed it.
apk = toml({**GOOD, "id": "arospkg", "url": "https://example.invalid/arospkg.zip"})
code, text, out = run({"demo.x86_64.v11.toml": toml(GOOD), "arospkg.x86_64.v11.toml": apk}, ["demo"])
ids = sorted(p["id"] for p in json.loads(last_old)["packages"]) if code == 0 else []
if ids != ["arospkg", "demo"]:
    fails.append(f"arospkg always in index.json: exit {code}, {ids}: {out}")

# An override is applied when the catalogue is made, not only on submission.
code, text, out = run({"demo.x86_64.v11.toml": toml(GOOD)}, overrides={"demo.x86_64.v11.toml": 'category = "game/action"\n'})
if code or json.loads(text)["packages"][0].get("category") != "game/action":
    fails.append(f"override at generation: exit {code}: {out}")
code, text, out = run({"demo.x86_64.v11.toml": toml(GOOD)}, overrides={"demo.x86_64.v11.toml": 'sha256 = "' + "b" * 64 + '"\n'})
if code == 0 or "may not change sha256" not in out:
    fails.append(f"override of sha256 must be refused: exit {code}")

if fails:
    print("\n".join(fails)); print(f"MKINDEX: {len(fails)} failed"); sys.exit(1)
print("MKINDEX: all refusals and publication rules hold")
