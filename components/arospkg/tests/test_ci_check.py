"""ci_check_index.py, the check an arospkg-index pull request gets, on a
scratch repository: it must refuse what submit refuses even when a pull
request is written by hand, fail on an archive it cannot read, and check a
change made only to an override. Host only."""
import hashlib, io, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
fails = []


def archive(readme=b"hello\n"):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr("Demo/", b""); z.writestr("Demo/ReadMe", readme); z.writestr("Demo.info", b"icon")
    return b.getvalue()


def manifest(data, version="1.0", url="https://example.invalid/demo.zip", extra=""):
    return (f'status = "approved"\nid = "demo"\nversion = "{version}"\nsummary = "Demo"\n'
            f'arch = "x86_64"\nabi = "v11"\nkind = "app"\nurl = "{url}"\nsize = {len(data)}\n'
            f'sha256 = "{hashlib.sha256(data).hexdigest()}"\ndepends = []\ndepends_checked = true\n'
            f'subdir = "Demo"\nicon = "Demo.info"\n{extra}')


def git(repo, *args):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *args], cwd=repo, check=True,
                   capture_output=True)


def check(repo, files):
    with tempfile.TemporaryDirectory() as local:
        for name, data in files.items():
            (Path(local) / name).write_bytes(data)
        r = subprocess.run([sys.executable, str(HERE / "tools/ci_check_index.py"), str(repo), "--base", "base",
                            "--use-local-copies", local, "--cache", str(Path(local) / "cache")],
                           capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def scenario(change, setup=None):
    """A repository whose base has demo 1.0, then `change` applied on top."""
    d = Path(tempfile.mkdtemp())
    (d / "manifests").mkdir()
    a = archive()
    (d / "manifests/demo.x86_64.v11.toml").write_text(manifest(a))
    if setup:
        setup(d)
    git(d, "init", "-q"); git(d, "add", "-A"); git(d, "commit", "-qm", "base"); git(d, "tag", "base")
    files = change(d, a)
    git(d, "add", "-A"); git(d, "commit", "-qm", "change")
    return check(d, files)


# Other bytes under the published version and revision, written by hand.
b = archive(b"changed\n")
code, out = scenario(lambda d, a: ((d / "manifests/demo.x86_64.v11.toml").write_text(manifest(b)), {"demo.zip": b})[1])
if code == 0 or "different bytes need a new revision" not in out:
    fails.append(f"same release, other bytes: {code} {out}")

# The same bytes as a new revision: accepted.
code, out = scenario(lambda d, a: ((d / "manifests/demo.x86_64.v11.toml").write_text(manifest(b, extra="revision = 1\n")), {"demo.zip": b})[1])
if code or "reviewed (suite" not in out:
    fails.append(f"new revision: {code} {out}")

# An archive that cannot be read, with a matching hash: a failed check.
junk = b"this is not an archive" * 10
code, out = scenario(lambda d, a: ((d / "manifests/demo.x86_64.v11.toml").write_text(manifest(junk, "1.1")), {"demo.zip": junk})[1])
if code == 0 or "archive: cannot be read" not in out:
    fails.append(f"unreadable archive: {code} {out}")

# Only an override changes: the entry is checked with it applied.
def override(d, a):
    (d / "overrides").mkdir()
    (d / "overrides/demo.x86_64.v11.toml").write_text('subdir = "Elsewhere"\n')
    return {"demo.zip": a}
code, out = scenario(override)
if code == 0 or "subdir: 'Elsewhere' is not in the archive" not in out:
    fails.append(f"override-only change: {code} {out}")

# Two uploads with the same file name do not stand in for each other.
c = archive(b"other\n")
def two(d, a):
    (d / "manifests/demo.x86_64.v11.toml").write_text(manifest(c, "2.0", "https://one.invalid/release.zip"))
    return {"release.zip": c}
code, out = scenario(two)
if code:
    fails.append(f"new version from another URL: {code} {out}")

# Comment-only edits must not fetch unchanged archives.
def comment_only(d, a):
    p = d / "manifests/demo.x86_64.v11.toml"
    p.write_text("# Documentation cleanup\n" + p.read_text() + "\n")
    return {}
code, out = scenario(comment_only)
if code or "cannot download" in out:
    fails.append(f"comment-only manifest: {code} {out}")

def base_override(d):
    (d / "overrides").mkdir()
    (d / "overrides/demo.x86_64.v11.toml").write_text('summary = "Overridden"\n')

def override_comment(d, a):
    p = d / "overrides/demo.x86_64.v11.toml"
    p.write_text("# Documentation cleanup\n" + p.read_text())
    return {}
code, out = scenario(override_comment, base_override)
if code or "cannot download" in out:
    fails.append(f"comment-only override: {code} {out}")

def remove_override(d, a):
    (d / "overrides/demo.x86_64.v11.toml").unlink()
    return {}
code, out = scenario(remove_override, base_override)
if code == 0 or "cannot download" not in out:
    fails.append(f"removed override must check: {code} {out}")

# review/ and history.json are the publish workflow's: a pull request that
# writes a passing record by hand fails, whoever it claims to be.
def forged_review(d, a):
    (d / "review").mkdir(exist_ok=True)
    (d / "review/demo.x86_64.v11.json").write_text('{"result": "passed"}\n')
    return {}
code, out = scenario(forged_review)
if code == 0 or "only the publish workflow writes" not in out:
    fails.append(f"hand-written review record: {code} {out}")
def forged_history(d, a):
    (d / "history.json").write_text('{"schema": 1, "variants": {}, "apps": {}}\n')
    return {}
code, out = scenario(forged_history)
if code == 0 or "only the publish workflow writes" not in out:
    fails.append(f"hand-written history: {code} {out}")
def bot_review(d, a):
    (d / "review").mkdir(exist_ok=True)
    (d / "review/demo.x86_64.v11.json").write_text('{"result": "passed"}\n')
    git(d, "add", "-A")
    subprocess.run(["git", "-c", "user.name=arospkg-index", "-c", "user.email=b@b", "commit", "-qm", "bot"],
                   cwd=d, check=True, capture_output=True)
    (d / "README").write_text("an unrelated change after it\n")
    return {}
code, out = scenario(bot_review)
if code == 0 or "only the publish workflow writes" not in out:
    fails.append(f"a pull request naming the workflow as author is refused all the same: {code} {out}")

# In the publish run, a record that reached main from anywhere else is not
# kept: its entry is reviewed again and the record rewritten.
d = Path(tempfile.mkdtemp())
(d / "manifests").mkdir()
good = archive()
(d / "manifests/demo.x86_64.v11.toml").write_text(manifest(good))
git(d, "init", "-q"); git(d, "add", "-A"); git(d, "commit", "-qm", "base"); git(d, "tag", "base")
(d / "review").mkdir()
(d / "review/demo.x86_64.v11.json").write_text('{"result": "passed", "forged": true}\n')
git(d, "add", "-A"); git(d, "commit", "-qm", "forged")
with tempfile.TemporaryDirectory() as local:
    (Path(local) / "demo.zip").write_bytes(good)
    r = subprocess.run([sys.executable, str(HERE / "tools/ci_check_index.py"), str(d), "--base", "base", "--publish",
                        "--use-local-copies", local, "--cache", str(Path(local) / "cache"),
                        "--out-dir", str(d), "--review-dir", str(d / "review")], capture_output=True, text=True)
rec = (d / "review/demo.x86_64.v11.json").read_text()
if "reviewed again" not in r.stdout or "forged" in rec or '"suite"' not in rec:
    fails.append(f"publish: a record from outside is reviewed again: {r.returncode} {r.stdout}{r.stderr} {rec}")

# --review-all: an unchanged entry whose upstream file changed stays Listed
# and does not stop the run; the trial catalogue says listed.
def upstream_changed(d, a):
    return {"demo.zip": archive(b"replaced upstream\n")}
d = Path(tempfile.mkdtemp())
(d / "manifests").mkdir()
(d / "manifests/demo.x86_64.v11.toml").write_text(manifest(archive()))
git(d, "init", "-q"); git(d, "add", "-A"); git(d, "commit", "-qm", "base")
with tempfile.TemporaryDirectory() as local:
    (Path(local) / "demo.zip").write_bytes(archive(b"replaced upstream\n"))
    out_dir = Path(local) / "out"
    r = subprocess.run([sys.executable, str(HERE / "tools/ci_check_index.py"), str(d), "--review-all",
                        "--use-local-copies", local, "--cache", str(Path(local) / "cache"),
                        "--out-dir", str(out_dir), "--review-dir", str(Path(local) / "review")],
                       capture_output=True, text=True)
    import json
    pk = json.loads((out_dir / "index-v2.json").read_text())["packages"] if (out_dir / "index-v2.json").exists() else []
    rec = json.loads((Path(local) / "review/demo.x86_64.v11.json").read_text()) \
        if (Path(local) / "review/demo.x86_64.v11.json").exists() else {}
if r.returncode or "stays Listed" not in r.stdout or not pk or pk[0]["review"] != {"status": "listed"} \
        or rec.get("result") != "failed":
    fails.append(f"review-all with a changed upstream file: {r.returncode} {r.stdout}{r.stderr} {pk} {rec}")

if fails:
    print("\n".join(fails)); print(f"CI-CHECK: {len(fails)} failed"); sys.exit(1)
print("CI-CHECK: same-release rule, unreadable archives and overrides are enforced")
