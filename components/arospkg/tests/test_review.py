#!/usr/bin/env python3
"""tools/review.py and the catalogue fields it feeds (mkindex.py): Reviewed
passes for a sound archive and is refused for each kind of defect; a record
counts only for the bytes and the metadata it was made with; runtime tests
are published only for the bytes they were made with; first publication
dates come from history.json and never from today's date by default.
Host only."""
import hashlib, io, json, os, shutil, struct, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "tools"))
import catalogue_history as ch  # noqa: E402
import mkindex  # noqa: E402
import review as rv  # noqa: E402

fails = []
TMP = Path(tempfile.mkdtemp(prefix="review-test-"))


def elf(machine, cls=2):
    """A 64-byte ELF header: enough for the CPU check, which reads no more."""
    h = bytearray(64)
    h[:4] = b"\x7fELF"; h[4] = cls; h[5] = 1; h[6] = 1
    h[16:18] = (1).to_bytes(2, "little")            # ET_REL, as AROS executables are
    h[18:20] = machine.to_bytes(2, "little")
    return bytes(h)


def zip_of(members, compression=zipfile.ZIP_STORED):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", compression) as z:
        for name, data in members:
            z.writestr(name, data)
    return b.getvalue()


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def lha_of(members):
    """Level-1 -lh0- headers with a real CRC, so lhasa extracts them."""
    out = b""
    for name, data in members:
        name = name.encode()
        body = (b"-lh0-" + struct.pack("<II", len(data), len(data)) + struct.pack("<I", 0x5A210000) +
                bytes([0x20, 1, len(name)]) + name + struct.pack("<H", crc16(data)) + b"A" + struct.pack("<H", 0))
        out += bytes([len(body), sum(body) & 0xFF]) + body + data
    return out + b"\x00"


GOOD = [("Demo/", b""), ("Demo/Demo", elf(62)), ("Demo/Demo.info", b"icon"), ("Demo/ReadMe", b"hello\n"),
        ("Demo.info", b"icon")]


def manifest(data, **kw):
    m = dict(status="approved", id="demo", version="1.0", summary="Demo", arch="x86_64", abi="v11",
             kind="app", url="https://example.invalid/demo.zip", size=len(data),
             sha256=hashlib.sha256(data).hexdigest(), depends=[], depends_checked=True,
             subdir="Demo", icon="Demo.info", launch={"executable": "Demo", "mode": "workbench"})
    m.update(kw)
    return {k: v for k, v in m.items() if v is not None}


def run(name, members, want_result, want_check=None, want_detail=None, make=zip_of, **kw):
    data = make(members)
    p = TMP / (name.replace(" ", "_") + (".zip" if make is zip_of else ".lha"))
    p.write_bytes(data)
    m = manifest(data, **kw)
    rec = rv.review(m, p, today="2026-10-14")
    bad = {c["name"]: c for c in rec["checks"] if c["result"] != "passed"}
    ok = rec["result"] == want_result
    if want_check:
        ok = ok and want_check in bad and (not want_detail or want_detail in bad[want_check]["detail"])
    if not ok:
        fails.append(f"{name}: want {want_result} {want_check or ''} {want_detail or ''}, got {rec['result']} "
                     f"{json.dumps(bad)}")
    return rec, m, p


rec, m, p = run("sound archive", GOOD, "passed")
if [c["name"] for c in rec["checks"]] != ["metadata", "download", "archive", "sizes", "paths", "layout", "cpu"]:
    fails.append(f"the suite runs every check: {rec['checks']}")
if [u["name"] for u in rec["unchecked"]] != ["abi"]:
    fails.append(f"the ABI is reported as not decidable, never guessed: {rec['unchecked']}")
if rec["sha256"] != m["sha256"] or rec["suite"] != rv.SUITE or rec["manifest_digest"] != rv.manifest_digest(m):
    fails.append("the record names the bytes, the suite and the metadata it covered")
run("sound LHA archive", [("Demo/Demo", elf(62)), ("Demo/Demo.info", b"i"), ("Demo.info", b"i")], "passed",
    make=lha_of)

# Each defect on its own.
run("wrong size and hash", GOOD, "failed", "download", sha256="0" * 64)
run("unreadable", [], "failed", "archive", "cannot be read", make=lambda _: b"not an archive at all")
run("name twice", GOOD + [("Demo/ReadMe", b"again\n")], "failed", "paths", "twice")
run("names equal but for case", GOOD + [("Demo/README", b"x")], "failed", "paths", "differ only in case")
run("file and drawer of one name", GOOD + [("Demo/Data", b"x"), ("Demo/Data/x", b"y")], "failed", "paths",
    "both a file and a drawer")
run("leaves the drawer", GOOD + [("Demo/../../S/Startup-Sequence", b"x")], "failed", "paths", "refuses this name")
run("names a volume", GOOD + [("SYS:C/Dir", b"x")], "failed", "paths", "refuses this name")
run("no icon", [m_ for m_ in GOOD if m_[0] != "Demo.info"], "failed", "layout", "icon")
run("no subdir", GOOD, "failed", "layout", "subdir", subdir="Other", launch=None, icon=None)
run("no program", [m_ for m_ in GOOD if m_[0] != "Demo/Demo"], "failed", "layout", "launch.executable")
run("program for i386", [("Demo/Demo", elf(3, 1)) if n == "Demo/Demo" else (n, d) for n, d in GOOD], "failed",
    "cpu", "is i386")
# The program the entry starts must be read; one that cannot be is a failed
# CPU check, never a skipped one.
real_heads = rv.member_heads
rv.member_heads = lambda path, names: {n: h for n, h in real_heads(path, names).items() if n != "Demo/Demo"}
run("program unreadable", GOOD, "failed", "cpu", "could not be read")
rv.member_heads = real_heads
run("only a 68k program", [("Demo/Demo", b"\x00\x00\x03\xf3" + b"\0" * 60) if n == "Demo/Demo" else (n, d)
                           for n, d in GOOD], "failed", "cpu", "68k")
run("bad metadata", GOOD, "failed", "metadata", "not a legal package id", id="Bad Id")
rec, *_ = run("no ELF at all", [("Fonts/", b""), ("Fonts/x.font", b"FONT")], "passed", subdir="Fonts",
              icon=None, launch=None, kind="data")
if [u["name"] for u in rec["unchecked"]] != ["cpu", "abi"]:
    fails.append(f"no ELF program: the CPU is reported as not checked, not as passed: {rec['unchecked']}")
rec, *_ = run("other CPU beside", GOOD + [("Demo/extra-i386", elf(3, 1))], "passed")
cpu = [c for c in rec["checks"] if c["name"] == "cpu"][0]
if "another CPU" not in cpu["detail"]:
    fails.append(f"a second CPU's file beside the program is reported: {cpu}")

# Sizes: recorded for the catalogue, and checked against what the client accepts.
rec, *_ = run("sizes recorded", GOOD, "passed")
if rec.get("sizes") != {"largest_member": len(GOOD[1][1]), "unpacked": sum(len(d) for n, d in GOOD if not n.endswith("/"))}:
    fails.append(f"the largest member and the unpacked total are recorded: {rec.get('sizes')}")
big = 70 * 1024 * 1024
deflated = lambda ms: zip_of(ms, zipfile.ZIP_DEFLATED)
rec, *_ = run("a member over 64 MiB", GOOD + [("Demo/Big", b"\0" * big)], "passed", make=deflated)
sz = [c for c in rec["checks"] if c["name"] == "sizes"][0]
if rec["sizes"]["largest_member"] != big or "clients before 0.6" not in sz["detail"]:
    fails.append(f"a member over 64 MiB passes and says which clients refuse it: {sz}")
run("a member over 128 MiB", GOOD + [("Demo/Huge", b"\0" * (129 * 1024 * 1024))], "failed", "sizes", "limit for one member",
    make=deflated)
run("an archive over 64 MiB", GOOD + [("Demo/Noise", os.urandom(65 * 1024 * 1024))], "failed", "sizes", "download limit")
run("an LHA member over 64 MiB", [("Demo/Demo", elf(62)), ("Demo/Demo.info", b"i"), ("Demo.info", b"i"),
                                  ("Demo/Big", b"\0" * big)], "failed", "sizes", "limit for one member", make=lha_of)

def zip_with_link(members, link, target):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        for name, data in members:
            z.writestr(name, data)
        info = zipfile.ZipInfo(link)
        info.external_attr = (0o120777 << 16)
        z.writestr(info, target)
    return b.getvalue()
run("a symbolic link", GOOD, "failed", "paths", "symbolic link", make=lambda m: zip_with_link(m, "Demo/etc", "/etc"))

# Records count only for what they covered.
rec, m, p = run("record", GOOD, "passed")
if not rv.record_applies(rec, m):
    fails.append("a passing record applies to its own entry")
if rv.record_applies(rec, {**m, "summary": "changed"}):
    fails.append("a changed value makes the record stale")
if rv.record_applies(rec, {**m, "sha256": "1" * 64}):
    fails.append("other bytes make the record stale")
if rv.record_applies({**rec, "result": "failed"}, m):
    fails.append("a failed record never counts")
import tomllib
a = tomllib.loads('id = "x"\nversion = "1"\n')
b = tomllib.loads('# a comment\nid    = "x"   # trailing\n\nversion = "1"\n')
if rv.manifest_digest(a) != rv.manifest_digest(b):
    fails.append("comments and layout do not change the digest")


# ------------------------------------------------- mkindex: the catalogue fields
def toml_of(m):
    out = []
    for k, v in m.items():
        if isinstance(v, dict):
            continue
        out.append(f"{k} = {json.dumps(v)}")
    for k, v in m.items():
        if isinstance(v, dict):
            out.append(f"[{k}]")
            out += [f"{a} = {json.dumps(b)}" for a, b in v.items()]
    return "\n".join(out) + "\n"


def index_repo(m, rec=None, tests=None, history=None):
    d = Path(tempfile.mkdtemp(prefix="review-idx-", dir=TMP))
    (d / "manifests").mkdir(); (d / "review").mkdir(); (d / "runtime-tests").mkdir()
    (d / "manifests/demo.x86_64.v11.toml").write_text(toml_of(m))
    if rec:
        (d / "review/demo.x86_64.v11.json").write_text(json.dumps(rec))
    if tests:
        (d / "runtime-tests/demo.x86_64.v11.toml").write_text(tests)
    if history is not None:
        (d / "history.json").write_text(json.dumps(history))
    return d


def generate(d, *extra):
    r = subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(d / "manifests"),
                        "--out-dir", str(d), "--no-archive-check", "--review-dir", str(d / "review"),
                        "--runtime-dir", str(d / "runtime-tests"), *extra], capture_output=True, text=True)
    pkgs = json.loads((d / "index-v2.json").read_text())["packages"] if (d / "index-v2.json").exists() else []
    return r, pkgs


rec, m, p = run("for mkindex", GOOD, "passed")
d = index_repo(m, rec)
r, pkgs = generate(d)
if r.returncode or pkgs[0]["review"].get("status") != "reviewed" or pkgs[0]["review"].get("unchecked") != ["abi"]:
    fails.append(f"a matching record: reviewed in the catalogue: {r.stdout}{r.stderr} {pkgs}")
if "review" in json.loads((d / "index.json").read_text())["packages"][0]:
    fails.append("index.json, for clients up to 0.3.2, carries no review fields")
d = index_repo({**m, "summary": "Changed later"}, rec)
r, pkgs = generate(d)
if pkgs[0]["review"] != {"status": "listed"}:
    fails.append(f"metadata changed after the review: listed again: {pkgs}")
forged = {**rec, "manifest_digest": "0" * 64}
d = index_repo(m, forged)
r, pkgs = generate(d)
if pkgs[0]["review"] != {"status": "listed"}:
    fails.append(f"a record for other metadata does not make it reviewed: {pkgs}")
d = index_repo(m)
r, pkgs = generate(d)
if pkgs[0]["review"] != {"status": "listed"} or pkgs[0]["runtime_tests"] != []:
    fails.append(f"no record: listed, and published all the same: {r.stdout}{r.stderr} {pkgs}")

T = lambda sha, date, result="passed", **kw: (
    "[[test]]\n" + "\n".join(f"{k} = {json.dumps(v)}" for k, v in dict(
        sha256=sha, date=date, system="AROS One 1.3", arch="x86_64", abi="v11", machine="QEMU (KVM)",
        result=result, checked=["install", "start", "remove"], note="window open", **kw).items()) + "\n")
d = index_repo(m, rec, T(m["sha256"], "2026-09-29") + T("1" * 64, "2026-09-20"))
r, pkgs = generate(d)
tests = pkgs[0]["runtime_tests"] if pkgs else None
if r.returncode or not tests or len(tests) != 1 or tests[0]["date"] != "2026-09-29" or "sha256" in tests[0]:
    fails.append(f"runtime tests: only those for these bytes are published: {r.stdout}{r.stderr} {tests}")
for name, bad in [("result", T(m["sha256"], "2026-09-29", result="works")),
                  ("date", T(m["sha256"], "29.09.2026")),
                  ("steps", T(m["sha256"], "2026-09-29").replace('"install"', '"played"')),
                  ("target", T(m["sha256"], "2026-09-29").replace('abi = "v11"', 'abi = "v1"')),
                  ("key", T(m["sha256"], "2026-09-29") + 'verified = true\n')]:
    d = index_repo(m, rec, bad)
    r, _ = generate(d)
    if r.returncode == 0 or "runtime-tests/demo.x86_64.v11.toml" not in r.stdout:
        fails.append(f"a malformed runtime test ({name}) stops publication: {r.returncode} {r.stdout}")

# First publication dates.
H = {"schema": 1, "variants": {}, "apps": {}}
d = index_repo(m, rec, history=H)
r, pkgs = generate(d, "--history", str(d / "history.json"), "--today", "2026-10-14", "--commit", "abc123")
h = json.loads((d / "history.json").read_text())
if r.returncode or pkgs[0].get("first_added") != "2026-10-14" or pkgs[0].get("app_first_added") != "2026-10-14" \
        or h["variants"]["demo/x86_64/v11"] != {"first_added": "2026-10-14", "commit": "abc123"}:
    fails.append(f"a new variant is dated the day it is first published: {r.stdout}{r.stderr} {pkgs} {h}")
H2 = {"schema": 1, "variants": {"demo/x86_64/v11": {"first_added": "2026-09-29", "commit": "old"}},
      "apps": {"demo": {"first_added": "2026-09-20", "commit": "older"}}}
d = index_repo({**m, "version": "2.0"}, None, history=H2)
r, pkgs = generate(d, "--history", str(d / "history.json"), "--today", "2026-10-14")
if pkgs[0].get("first_added") != "2026-09-29" or pkgs[0].get("app_first_added") != "2026-09-20":
    fails.append(f"a new version, no review, a return after removal: the first date stays: {pkgs}")
d = index_repo(m, rec)
r, pkgs = generate(d)
if "first_added" in pkgs[0]:
    fails.append("without a history nothing is dated, rather than everything today")
d = index_repo(m, rec)
r, _ = generate(d, "--history", str(d / "missing.json"))
if r.returncode == 0 or "not made up from today's date" not in (r.stdout + r.stderr):
    fails.append(f"a named history that is missing stops publication: {r.returncode} {r.stdout}{r.stderr}")
try:
    ch.load_text = None
    bad = TMP / "bad-history.json"
    bad.write_text(json.dumps({"schema": 1, "variants": {"x/y/z": {"first_added": "today"}}, "apps": {}}))
    ch.load(bad)
    fails.append("a history with a malformed date is refused")
except ValueError:
    pass

shutil.rmtree(TMP, ignore_errors=True)
if fails:
    print("\n".join(fails)); print(f"REVIEW: {len(fails)} failed"); sys.exit(1)
print("REVIEW: the suite, its records, runtime tests and first dates hold")
