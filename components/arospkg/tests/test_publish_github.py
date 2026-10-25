"""submit --pr for an author who cannot push to the catalogue repository,
against a stand-in gh (tests/fakegh/gh) and local bare repositories: the
fork is made when missing and reused when present, the branch goes only to
the fork, the pull request names the upstream repository, its base and
me:branch as head, and submitting the same release again opens no second
pull request. GitHub itself is not involved; see the report for the real run."""
import json, os, subprocess, sys, tempfile, zipfile, io
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
UP = "upstream/arospkg-index"
fails = []


def git(cwd, *a):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", *a], cwd=cwd,
                          check=True, capture_output=True, text=True).stdout


def archive(readme=b"hi\n", rev=None):
    man = ('id = "demo"\nversion = "1.0"\n' + (f"revision = {rev}\n" if rev else "") +
           'summary = "Demo"\nkind = "app"\narch = "x86_64"\nabi = "v11"\nsubdir = "Demo"\n'
           'icon = "Demo.info"\ndepends = []\ndepends_checked = true\n')
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w") as z:
        for n, d in (("Demo/", b""), ("Demo/ReadMe", readme), ("Demo.info", b"i"),
                     (".arospkg/", b""), (".arospkg/manifest.toml", man.encode())):
            z.writestr(n, d)
    return b.getvalue()


def setup(d, fork_exists=False, squatter=False):
    fake = d / "fake"; (fake / "repos/upstream").mkdir(parents=True)
    seed = d / "seed"; (seed / "manifests").mkdir(parents=True)
    (seed / "manifests/.keep").write_text("")
    git(seed, "init", "-q", "-b", "main"); git(seed, "add", "-A"); git(seed, "commit", "-qm", "seed")
    git(d, "clone", "-q", "--bare", str(seed), str(fake / "repos" / (UP + ".git")))
    state = {"me": "author", "prs": [],
             "repos": {UP: {"fork": False, "permissions": {"push": False}}}}
    if fork_exists or squatter:
        (fake / "repos/author").mkdir()
        git(d, "clone", "-q", "--bare", str(seed), str(fake / "repos/author/arospkg-index.git"))
        state["repos"]["author/arospkg-index"] = (
            {"fork": True, "parent": {"full_name": UP}, "permissions": {"push": True}} if fork_exists
            else {"fork": False, "permissions": {"push": True}})
    (fake / "state.json").write_text(json.dumps(state))
    return fake


def submit(fake, data, d):
    z = d / "demo.zip"; z.write_bytes(data)
    # The commit carries the user's git identity; a CI runner has none.
    env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@t", **os.environ,
           "FAKEGH": str(fake), "PATH": f"{HERE / 'tests/fakegh'}:{os.environ['PATH']}"}
    r = subprocess.run([sys.executable, str(HERE / "tools/apkg-pack"), "submit",
                        "https://example.invalid/demo.zip", "--pr", "--repo", UP, "--use-local-copy", str(z)],
                       capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def branches(fake, full):
    return git(fake / "repos" / (full + ".git"), "branch", "--format=%(refname:short)").split()


def calls(fake):
    return [json.loads(l) for l in (fake / "calls.log").read_text().splitlines()]


with tempfile.TemporaryDirectory() as t:
    d = Path(t) / "a"; d.mkdir()
    fake = setup(d)
    code, out = submit(fake, archive(), d)
    st = json.loads((fake / "state.json").read_text())
    pr = st["prs"][0] if st["prs"] else {}
    if code or "forked upstream/arospkg-index to author/arospkg-index" not in out:
        fails.append(f"first submission without push rights: {code} {out}")
    if branches(fake, UP) != ["main"]:
        fails.append(f"something was pushed upstream: {branches(fake, UP)}")
    if "submit/demo-x86_64-v11-1.0-r0" not in branches(fake, "author/arospkg-index"):
        fails.append(f"branch not in the fork: {branches(fake, 'author/arospkg-index')}")
    if (pr.get("repo"), pr.get("owner"), pr.get("base"), pr.get("branch")) != \
            (UP, "author", "main", "submit/demo-x86_64-v11-1.0-r0"):
        fails.append(f"pull request: {pr}")
    files = git(fake / "repos/author/arospkg-index.git", "show", "--name-only", "--format=",
                "submit/demo-x86_64-v11-1.0-r0").split()
    if files != ["manifests/demo.x86_64.v11.toml"]:
        fails.append(f"the commit carries more than the manifest: {files}")

    # The same release again, its pull request still open: no second one.
    code, out = submit(fake, archive(), d)
    st = json.loads((fake / "state.json").read_text())
    if code or "already open" not in out or len(st["prs"]) != 1 or \
            sum(1 for c in calls(fake) if c[:2] == ["repo", "fork"]) != 1:
        fails.append(f"resubmission: {code} {out} prs={len(st['prs'])}")

    # An existing fork is used, not forked again.
    d2 = Path(t) / "b"; d2.mkdir()
    fake2 = setup(d2, fork_exists=True)
    code, out = submit(fake2, archive(b"r1\n", rev=1), d2)
    if code or "using your fork" not in out or any(c[:2] == ["repo", "fork"] for c in calls(fake2)) \
            or "submit/demo-x86_64-v11-1.0-r1" not in branches(fake2, "author/arospkg-index"):
        fails.append(f"existing fork: {code} {out}")

    # A repository of that name which is not a fork of upstream: refused.
    d3 = Path(t) / "c"; d3.mkdir()
    fake3 = setup(d3, squatter=True)
    code, out = submit(fake3, archive(), d3)
    if code == 0 or "is not a fork of" not in out or json.loads((fake3 / "state.json").read_text())["prs"]:
        fails.append(f"same-name repository that is not a fork: {code} {out}")

if fails:
    print("\n".join(fails)); print(f"PUBLISH-GITHUB: {len(fails)} failed"); sys.exit(1)
print("PUBLISH-GITHUB: fork made or reused, pushed only there, one pull request per release")
