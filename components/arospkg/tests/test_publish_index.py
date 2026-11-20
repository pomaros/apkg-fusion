"""publish_index.py, the publish workflow's step, on scratch repositories
(a bare "origin" and pull requests merged into it): two merges in quick
succession, a refused push followed by another attempt, and pull requests
that smuggle review records or history, or fake the workflow's commit.
Host only."""
import hashlib, io, json, os, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
TOOL = HERE / "tools/publish_index.py"
fails = []


def archive(name, readme=b"hello\n"):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        z.writestr(f"{name}/", b""); z.writestr(f"{name}/ReadMe", readme); z.writestr(f"{name}.info", b"icon")
    return b.getvalue()


def manifest(pid, data, version="1.0"):
    name = pid.capitalize()
    return (f'status = "approved"\nid = "{pid}"\nversion = "{version}"\nsummary = "{name}"\n'
            f'arch = "x86_64"\nabi = "v11"\nkind = "app"\nurl = "https://example.invalid/{pid}-{version}.zip"\n'
            f'size = {len(data)}\nsha256 = "{hashlib.sha256(data).hexdigest()}"\ndepends = []\n'
            f'depends_checked = true\nsubdir = "{name}"\nicon = "{name}.info"\n')


def git(repo, *args, user="t"):
    r = subprocess.run(["git", "-c", f"user.name={user}", "-c", "user.email=t@t", *args], cwd=repo,
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"git {args}: {r.stderr}")
    return r.stdout.strip()


class World:
    """origin (bare), the archives at their URLs, and a clone for merging pull requests."""

    def __init__(self):
        self.root = Path(tempfile.mkdtemp())
        self.origin = self.root / "origin.git"
        self.files = self.root / "files"; self.files.mkdir()
        git(self.root, "init", "-q", "--bare", "-b", "main", str(self.origin))
        self.dev = self.root / "dev"
        git(self.root, "clone", "-q", str(self.origin), str(self.dev))
        git(self.dev, "checkout", "-q", "-b", "main")
        (self.dev / "manifests").mkdir()
        self.package("demo", "1.0")
        self.merge("base")
        self.run_no = 0

    def sync(self):
        if self.sha("main"):
            git(self.dev, "pull", "-q", "--rebase", "origin", "main")

    def package(self, pid, version, readme=b"hello\n"):
        """A pull request's manifest (on the current main)."""
        if not git(self.dev, "status", "--porcelain"):
            self.sync()
        data = archive(pid.capitalize(), readme)
        (self.files / f"{pid}-{version}.zip").write_bytes(data)
        (self.dev / f"manifests/{pid}.x86_64.v11.toml").write_text(manifest(pid, data, version))

    def merge(self, msg, user="t"):
        """A pull request merged into main (committed on the up-to-date main and pushed)."""
        git(self.dev, "add", "-A"); git(self.dev, "commit", "-qm", msg, user=user)
        git(self.dev, "push", "-q", "origin", "HEAD:main")
        return git(self.dev, "rev-parse", "HEAD")

    def sha(self, ref):
        r = subprocess.run(["git", "rev-parse", "--verify", "--quiet", ref], cwd=self.origin,
                           capture_output=True, text=True)
        return r.stdout.strip() or None

    def publish(self, start=None, base=None, attempts=3, before_push=None):
        """One workflow run: a fresh checkout of `start`, then the step."""
        self.run_no += 1
        co = self.root / f"run{self.run_no}"
        git(self.root, "clone", "-q", str(self.origin), str(co))
        if start:
            git(co, "checkout", "-q", "--detach", start)
        cmd = [sys.executable, str(TOOL), str(co), "--remote", str(self.origin), "--attempts", str(attempts),
               "--use-local-copies", str(self.files), "--cache", str(self.root / f"cache{self.run_no}")]
        if start:
            cmd += ["--start", start]
        if base:
            cmd += ["--base", base]
        if before_push:
            cmd += ["--before-push", before_push]
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def show(self, path, ref="main"):
        r = subprocess.run(["git", "show", f"{ref}:{path}"], cwd=self.origin, capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else None

    def state(self, pid):
        """(catalogue version, review status) of pid on origin main."""
        idx = json.loads(self.show("index-v2.json") or '{"packages": []}')
        for p in idx["packages"]:
            if p["id"] == pid:
                return p["version"], p["review"]["status"]
        return None


def merge_in_other_clone(w, pid, version, msg):
    """Shell command for --before-push: another pull request reaches main while the run works."""
    script = w.root / f"merge-{pid}.py"
    script.write_text(
        "import sys; sys.path.insert(0, %r)\n" % str(Path(__file__).parent)
        + "import test_publish_index as t\n"
        + f"w = t.World.__new__(t.World); w.root = t.Path({str(w.root)!r}); w.origin = w.root / 'origin.git'\n"
        + "w.files = w.root / 'files'; w.dev = w.root / 'dev'\n"
        + f"w.package({pid!r}, {version!r}); w.merge({msg!r})\n")
    return f"{sys.executable} {script}"


if __name__ == "__main__":
    # The first run: no `published` branch yet, everything is checked.
    w = World()
    code, out = w.publish()
    if code or w.state("demo") != ("1.0", "reviewed") or w.sha("published") != w.sha("main"):
        fails.append(f"first publication: {code} {out}")

    # Two merges 12 s apart. Run 1 (for PR 1) reviews demo 1.1, but PR 2 lands
    # before its push. With the push's `before` as base, as the workflow had
    # it, run 2 sees only PR 2 and demo 1.1 stays Listed:
    pr1 = (w.package("demo", "1.1"), w.merge("PR 1: demo 1.1"))[1]
    before_pr2 = pr1
    w.package("other", "1.0"); pr2 = w.merge("PR 2: other 1.0")
    old = w.root / "old"
    git(w.root, "clone", "-q", str(w.origin), str(old))
    r = subprocess.run([sys.executable, str(HERE / "tools/ci_check_index.py"), str(old), "--base", before_pr2,
                        "--publish", "--out-dir", str(old), "--review-dir", str(old / "review"),
                        "--use-local-copies", str(w.files), "--cache", str(w.root / "cache-old")],
                       capture_output=True, text=True)
    v2 = json.loads((old / "index-v2.json").read_text())["packages"]
    demo = [p for p in v2 if p["id"] == "demo"][0]
    if (demo["version"], demo["review"]["status"]) != ("1.1", "listed"):
        fails.append(f"reproduction: the old base should leave demo 1.1 Listed: {demo} {r.stdout}")

    # Run 1 with one attempt: its push is refused, nothing moves.
    w2 = World(); w2.publish()
    mark0, main0 = w2.sha("published"), w2.sha("main")
    w2.package("demo", "1.1"); a = w2.merge("PR 1: demo 1.1")
    code, out = w2.publish(start=a, attempts=1, before_push=merge_in_other_clone(w2, "other", "1.0", "PR 2"))
    if code == 0 or "push refused" not in out or w2.sha("published") != mark0 or w2.show("index-v2.json") != \
            subprocess.run(["git", "show", f"{main0}:index-v2.json"], cwd=w2.origin, capture_output=True,
                           text=True).stdout:
        fails.append(f"refused push must leave published and the catalogue: {code} {out}")
    # Run 2, for PR 2: base is `published`, so demo 1.1 is reviewed too.
    b = w2.sha("main")
    code, out = w2.publish(start=b)
    if code or w2.state("demo") != ("1.1", "reviewed") or w2.state("other") != ("1.0", "reviewed") \
            or w2.sha("published") != w2.sha("main"):
        fails.append(f"two quick merges: both reviewed: {code} {out} {w2.state('demo')} {w2.state('other')}")
    rec = json.loads(w2.show("review/demo.x86_64.v11.json") or "{}")
    data = (w2.files / "demo-1.1.zip").read_bytes()
    if rec.get("sha256") != hashlib.sha256(data).hexdigest():
        fails.append(f"review record of demo is not 1.1's: {rec}")
    # A later run with nothing new changes nothing.
    code, out = w2.publish(start=w2.sha("main"))
    if code or "catalogue unchanged" not in out:
        fails.append(f"rerun: {code} {out}")

    # The same race inside one run: the refused attempt is followed by one
    # on the new main, which publishes both.
    w3 = World(); w3.publish()
    w3.package("demo", "1.1"); a = w3.merge("PR 1")
    hook = merge_in_other_clone(w3, "other", "1.0", "PR 2")
    once = w3.root / "once.sh"
    once.write_text(f"[ -e {w3.root}/done ] && exit 0; touch {w3.root}/done; {hook}\n")
    code, out = w3.publish(start=a, attempts=2, before_push=f"sh {once}")
    if code or out.count("attempt ") != 2 or w3.state("demo") != ("1.1", "reviewed") \
            or w3.state("other") != ("1.0", "reviewed"):
        fails.append(f"retry after a refused push: {code} {out}")

    # A run that fails its check publishes nothing; the next one, after the
    # fix, publishes everything since `published`.
    w4 = World(); w4.publish()
    mark0 = w4.sha("published")
    w4.package("demo", "1.1")
    (w4.files / "demo-1.1.zip").write_bytes(b"not the archive")  # the URL serves other bytes
    a = w4.merge("PR 1: demo 1.1")
    code, out = w4.publish(start=a)
    if code == 0 or w4.sha("published") != mark0:
        fails.append(f"failed check must publish nothing: {code} {out}")
    w4.package("demo", "1.1"); w4.package("other", "1.0"); b = w4.merge("fix: demo 1.1 again, other 1.0")
    code, out = w4.publish(start=b)
    if code or w4.state("demo") != ("1.1", "reviewed") or w4.state("other") != ("1.0", "reviewed"):
        fails.append(f"rerun after a failed check: {code} {out}")

    # Smuggling: PR 1 brings a passing review record for its own manifest
    # (with bytes nobody reviewed) and its run's push is refused. PR 2 does
    # not touch it. Run 2 must still see the record as changed since
    # `published`, throw it away and review demo itself.
    w5 = World(); w5.publish()
    w5.package("demo", "1.1")
    forged = {"schema": 1, "suite": 99, "id": "demo", "arch": "x86_64", "abi": "v11", "result": "passed",
              "forged": True}
    (w5.dev / "review").mkdir(exist_ok=True)
    (w5.dev / "review/demo.x86_64.v11.json").write_text(json.dumps(forged))
    a = w5.merge("PR 1: demo 1.1 with a record")
    code, out = w5.publish(start=a, attempts=1, before_push=merge_in_other_clone(w5, "other", "1.0", "PR 2"))
    code, out = w5.publish(start=w5.sha("main"))
    rec = w5.show("review/demo.x86_64.v11.json") or ""
    if code or "reviewed again" not in out or "forged" in rec or w5.state("demo") != ("1.1", "reviewed"):
        fails.append(f"smuggled review record after a refused push: {code} {out} {rec}")

    # A smuggled history.json fails the run, whichever run sees it.
    w6 = World(); w6.publish()
    mark0 = w6.sha("published")
    w6.package("demo", "1.1")
    (w6.dev / "history.json").write_text('{"schema": 1, "variants": {}, "apps": {}}\n')
    a = w6.merge("PR 1 with history")
    w6.package("other", "1.0"); b = w6.merge("PR 2")
    code, out = w6.publish(start=b)
    if code == 0 or "only the publish workflow writes" not in out or w6.sha("published") != mark0:
        fails.append(f"smuggled history.json: {code} {out}")

    # A pull request carrying a commit that looks like the workflow's (its
    # author and message) does not move the base: the record in it is still
    # seen as changed since `published`.
    w7 = World(); w7.publish()
    w7.package("demo", "1.1")
    (w7.dev / "review").mkdir(exist_ok=True)
    (w7.dev / "review/demo.x86_64.v11.json").write_text(json.dumps(forged))
    a = w7.merge(f"Regenerate the catalogue from {w7.sha('main')[:12]}", user="arospkg-index")
    (w7.dev / "README").write_text("unrelated\n"); b = w7.merge("PR: readme")
    code, out = w7.publish(start=b)
    rec = w7.show("review/demo.x86_64.v11.json") or ""
    if code or "reviewed again" not in out or "forged" in rec or w7.state("demo") != ("1.1", "reviewed"):
        fails.append(f"fake workflow commit: {code} {out} {rec}")

    # A run started for a commit that is not on main publishes nothing.
    code, out = w7.publish(start=w7.sha("main"))  # fine
    stray = w7.root / "stray"
    git(w7.root, "clone", "-q", str(w7.origin), str(stray))
    (stray / "x").write_text("x"); git(stray, "add", "x"); git(stray, "commit", "-qm", "x")
    git(stray, "push", "-q", "origin", "HEAD:refs/heads/side")
    code, out = w7.publish(start=git(stray, "rev-parse", "HEAD"))
    if code == 0 or "is not on main" not in out:
        fails.append(f"start not on main: {code} {out}")

    if fails:
        print("\n\n".join(fails)); print(f"PUBLISH-INDEX: {len(fails)} failed"); sys.exit(1)
    print("PUBLISH-INDEX: quick merges, retries, failed checks and smuggled records are handled")
