#!/usr/bin/env python3
"""Publish the arospkg-index catalogue after a merge (the publish workflow's step).

    publish_index.py INDEX_DIR --remote URL [--start SHA] [--base REF]
                     [--review-all] [--attempts N] [--cache DIR]

Reviews what changed since the catalogue last published, regenerates the
catalogue files with ci_check_index.py --publish and pushes them in one
atomic push of two refs: the catalogue commit to main, and the same commit
to the branch `published`. `published` therefore names the last commit
whose catalogue is on main, written by this step and nothing else; a pull
request cannot move it, and neither an author name nor a commit message
in the history is taken as a sign of what was published.

The comparison base is `published` (--base overrides it, for a manual run;
without either, the whole index is checked). So a run whose push was
refused because main moved leaves `published` where it was, and the next
run reviews its changes together with the newer ones. The push is never
forced: a refused push means main has moved, and the step fetches the new
main and runs again, up to --attempts times; an older run cannot replace a
newer catalogue.

The token for the push is taken from the environment variable TOKEN (if
set, the push uses it as x-access-token); it is removed from the
environment of the check, which reads untrusted archives.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MARK = "published"
GENERATED = ["index.json", "index-v2.json", "index-v3.json", "history.json", "review"]
BOT = ["-c", "user.name=arospkg-index",
       "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com"]


def git(repo, *args, check=True, env=None):
    r = subprocess.run(["git", *args], cwd=repo, capture_output=True, text=True, env=env)
    if check and r.returncode:
        raise SystemExit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r


def remote_sha(repo, remote, ref):
    """The commit `ref` names at the remote, or None when there is no such branch."""
    out = git(repo, "ls-remote", "--heads", remote, ref).stdout.split()
    return out[0] if out else None


def fetch(repo, remote, sha=None):
    """Fetch main and `published` from the remote; sha: also this commit."""
    git(repo, "fetch", "-q", remote, f"+refs/heads/main:refs/publish/main")
    if remote_sha(repo, remote, MARK):
        git(repo, "fetch", "-q", remote, f"+refs/heads/{MARK}:refs/publish/{MARK}")
    else:
        git(repo, "update-ref", "-d", f"refs/publish/{MARK}", check=False)
    if sha and git(repo, "cat-file", "-e", f"{sha}^{{commit}}", check=False).returncode:
        git(repo, "fetch", "-q", remote, sha)


def local_sha(repo, ref):
    r = git(repo, "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}", check=False)
    return r.stdout.strip() or None


def attempt(repo, remote, start, args, push_url, env):
    """One run at commit `start`: 0 published (or nothing to publish), 1
    refused because main moved, 2 the check failed."""
    git(repo, "checkout", "-q", "--force", "--detach", start)
    # Files a refused attempt generated must not count as this one's.
    git(repo, "clean", "-fdq", "--", *GENERATED)
    mark = local_sha(repo, f"refs/publish/{MARK}")
    base = args.base or mark
    if base:
        print(f"base: {base}" + (" (published)" if base == mark else " (given)"), flush=True)
    else:
        print("base: none (no published branch yet); checking every manifest", flush=True)
    cmd = [sys.executable, str(HERE / "ci_check_index.py"), str(repo), "--publish", "--out-dir", str(repo),
           "--review-dir", str(Path(repo) / "review"), "--commit", start, "--cache", args.cache]
    if base:
        cmd += ["--base", base]
    if args.review_all:
        cmd.append("--review-all")
    if args.use_local_copies:
        cmd += ["--use-local-copies", args.use_local_copies]
    check_env = {k: v for k, v in env.items() if k != "TOKEN"}
    if subprocess.run(cmd, cwd=repo, env=check_env).returncode:
        return 2
    git(repo, "add", "--", *GENERATED)
    if git(repo, "diff", "--cached", "--quiet", check=False).returncode == 0:
        print("catalogue unchanged", flush=True)
    else:
        git(repo, *BOT, "commit", "-q", "-m", f"Regenerate the catalogue from {start[:12]}")
    head = local_sha(repo, "HEAD")
    if head == mark:
        return 0
    if args.before_push:
        subprocess.run(args.before_push, shell=True, check=True)
    # Both refs or neither: main only fast-forwards (no force), so a moved
    # main refuses the push and `published` stays at the last catalogue.
    r = git(repo, "push", "--atomic", "--porcelain", push_url, "HEAD:refs/heads/main", f"HEAD:refs/heads/{MARK}",
            check=False, env=env)
    if r.returncode == 0:
        print(f"published {head[:12]} (main and {MARK})", flush=True)
        return 0
    print(f"push refused (main has moved): {r.stdout.strip()} {r.stderr.strip()}", flush=True)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("index_dir")
    ap.add_argument("--remote", required=True, help="the repository URL (fetch, and push without TOKEN)")
    ap.add_argument("--start", help="the commit that started the run (the workflow's GITHUB_SHA): it must be on main; "
                    "main, which contains it, is what gets published")
    ap.add_argument("--base", help="compare with this commit instead of the published branch")
    ap.add_argument("--review-all", action="store_true")
    ap.add_argument("--attempts", type=int, default=3)
    ap.add_argument("--cache", default=".cache/ci-archives")
    ap.add_argument("--use-local-copies", metavar="DIR", help=argparse.SUPPRESS)
    ap.add_argument("--before-push", metavar="CMD", help=argparse.SUPPRESS)  # tests: main moves meanwhile
    args = ap.parse_args()
    repo = str(Path(args.index_dir).resolve())
    args.cache = str(Path(args.cache).resolve())
    env = dict(os.environ)
    push_url = args.remote
    if env.get("TOKEN") and args.remote.startswith("https://"):
        push_url = "https://x-access-token:" + env["TOKEN"] + "@" + args.remote[len("https://"):]
    for n in range(1, args.attempts + 1):
        fetch(repo, args.remote, args.start if n == 1 else None)
        main_sha = local_sha(repo, "refs/publish/main")
        if n == 1 and args.start and args.start != main_sha:
            if git(repo, "merge-base", "--is-ancestor", args.start, main_sha, check=False).returncode:
                raise SystemExit(f"{args.start[:12]} is not on main ({main_sha[:12]}); nothing published")
            print(f"main has moved past {args.start[:12]}; its changes are published with main's", flush=True)
        # Always the newest main: everything since `published` is in it.
        print(f"attempt {n}: publishing {main_sha[:12]}", flush=True)
        rc = attempt(repo, args.remote, main_sha, args, push_url, env)
        if rc == 0:
            return
        if rc == 2:
            raise SystemExit("the check failed; nothing published")
    raise SystemExit(f"main kept moving: not published after {args.attempts} attempts")

if __name__ == "__main__":
    main()
