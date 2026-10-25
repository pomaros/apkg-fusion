#!/usr/bin/env python3
"""Check an arospkg-index checkout the way its pull requests are checked.

    ci_check_index.py INDEX_DIR [--base REF] [--out-dir DIR] [--cache DIR]
                      [--review-dir DIR] [--review-all] [--commit SHA]

For every manifest whose TOML data changed since REF (all without --base): download
the archive at its url, within the client's limits, and run the automatic
review (review.py: size and SHA-256, a readable archive, safe and distinct
member names, subdir, icon and launch, the CPU of its programs). Then
generate both catalogue files with mkindex.py into OUT_DIR, with the review
records, the runtime tests and the first publication dates. Exit 1 on any
problem.

--review-dir: where the review records are written (the publish workflow
gives arospkg-index/review). --review-all: review every published manifest
and record each result; a failure leaves that entry Listed instead of
failing the run, since nothing about it was changed by anyone here.

review/ and history.json are written by the publish workflow only. A pull
request that changes them fails, whoever its commits name as author:
Reviewed is what the suite found, not what a pull request says. In the
publish run (--publish), a change to history.json that reached main
anyway fails, and every review record that changed is thrown away and its
entry reviewed again.

Nothing from the checkout is executed: manifests are read as TOML data;
archives are listed and their members' first bytes read through a pipe,
never unpacked to disk or run. That is what lets this run
on an untrusted pull request without secrets.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import mkindex  # noqa: E402
import review as rv  # noqa: E402
DOWNLOAD_MAX = 64 * 1024 * 1024
TIMEOUT = 300


def released(index_dir, base):
    """(id, arch, abi) -> (version, revision, sha256) of every approved manifest at base."""
    out = {}
    ls = subprocess.run(["git", "ls-tree", "--name-only", base, "manifests/"], cwd=index_dir,
                        capture_output=True, text=True, check=True).stdout.split()
    for path in ls:
        if not path.endswith(".toml"):
            continue
        text = subprocess.run(["git", "show", f"{base}:{path}"], cwd=index_dir,
                              capture_output=True, text=True, check=True).stdout
        try:
            m = tomllib.loads(text)
        except Exception:
            continue
        if m.get("status") == "approved" and m.get("sha256"):
            out[(m.get("id"), m.get("arch"), m.get("abi"))] = (m.get("version"), m.get("revision", 0), m["sha256"])
    return out


def metadata_changed(index_dir, path, base):
    """Comments/formatting are not changes; invalid data still needs checking."""
    old = subprocess.run(["git", "show", f"{base}:{path}"], cwd=index_dir,
                         capture_output=True, text=True)
    try:
        before = tomllib.loads(old.stdout) if old.returncode == 0 else None
        current = index_dir / path
        after = tomllib.loads(current.read_text(encoding="utf-8")) if current.exists() else None
        return before != after
    except (OSError, UnicodeError, tomllib.TOMLDecodeError):
        return True


def changed(index_dir, base):
    ancestor = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=index_dir,
                              capture_output=True, text=True, check=True).stdout.strip()
    out = subprocess.run(["git", "diff", "--name-only", "-z", "--diff-filter=AMRD",
                          f"{ancestor}..HEAD", "--", "manifests/", "overrides/"],
                         cwd=index_dir, capture_output=True, text=True, check=True).stdout
    names = {Path(path).name for path in out.split("\0") if path
             and metadata_changed(index_dir, path, ancestor)}
    return sorted(index_dir / "manifests" / n for n in names if (index_dir / "manifests" / n).exists())


# Written by the publish workflow only.
GENERATED = ("review/", "history.json")


def generated_changes(index_dir, base):
    """Paths under review/ and history.json changed between base and HEAD.
    Who claims to have written them does not matter: an author name is free
    text."""
    ancestor = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=index_dir,
                              capture_output=True, text=True, check=True).stdout.strip()
    out = subprocess.run(["git", "diff", "--name-only", f"{ancestor}..HEAD", "--", *GENERATED],
                         cwd=index_dir, capture_output=True, text=True, check=True).stdout
    return [ln for ln in out.splitlines() if ln]


def safe_name(m):
    """Whether the variant's file name can be built from its id, CPU and ABI:
    an id with '/' or '..' must not name a path outside review/."""
    import catalogue as cat
    return bool(cat.ID_RE.match(str(m.get("id", "")))) and m.get("arch") in mkindex.ARCHES \
        and m.get("abi") in mkindex.ABIS


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "arospkg-index-check"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        data = r.read(DOWNLOAD_MAX + 1)
    if len(data) > DOWNLOAD_MAX:
        raise ValueError(f"larger than {DOWNLOAD_MAX} bytes")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("index_dir")
    ap.add_argument("--base", help="check only manifests changed since this git ref")
    ap.add_argument("--out-dir", default=None, help="where the trial catalogue goes (default: a temporary directory)")
    ap.add_argument("--cache", default=".cache/ci-archives")
    ap.add_argument("--use-local-copies", metavar="DIR", help=argparse.SUPPRESS)
    ap.add_argument("--review-dir", help="write review records here (default: a temporary copy of review/)")
    ap.add_argument("--review-all", action="store_true", help="review every published manifest")
    ap.add_argument("--commit", default="", help="the commit being published (history.json)")
    ap.add_argument("--publish", action="store_true",
                    help="the run after a merge: review/ and history.json in the checkout are the workflow's")
    ap.add_argument("--today", help=argparse.SUPPRESS)
    a = ap.parse_args()
    index_dir = Path(a.index_dir).resolve()
    base_ok = a.base and subprocess.run(["git", "rev-parse", "--verify", "--quiet", a.base + "^{commit}"],
                                        cwd=index_dir, capture_output=True).returncode == 0
    if a.base and not base_ok:
        print(f"{a.base}: not a commit here (a first push?); checking every manifest")
    files = changed(index_dir, a.base) if base_ok and not a.review_all \
        else sorted((index_dir / "manifests").glob("*.toml"))
    cache = Path(a.cache).resolve()
    base_entries = released(index_dir, a.base) if base_ok else {}
    problems = 0
    redo = set()
    if base_ok:
        for path in generated_changes(index_dir, a.base):
            if a.publish and path.startswith("review/"):
                # Not trusted, not kept: the entry is reviewed again below.
                redo.add(Path(path).stem)
                (index_dir / path).unlink(missing_ok=True)
                print(f"{path}: changed outside this workflow; reviewed again")
            else:
                print(f"{path}: only the publish workflow writes review/ and history.json")
                problems += 1
        if redo:
            for f in sorted((index_dir / "manifests").glob("*.toml")):
                try:
                    m = tomllib.loads(f.read_text(encoding="utf-8"))
                    if f"{m.get('id')}.{m.get('arch')}.{m.get('abi')}" in redo and f not in files:
                        files.append(f)
                except Exception:
                    pass
    import shutil
    import tempfile
    work = Path(tempfile.mkdtemp(prefix="ci-review-"))
    import atexit
    atexit.register(shutil.rmtree, work, True)
    review_dir = Path(a.review_dir).resolve() if a.review_dir else work / "review"
    if not a.review_dir and (index_dir / "review").is_dir():
        shutil.copytree(index_dir / "review", review_dir)
    review_dir.mkdir(parents=True, exist_ok=True)
    listed = []
    for f in files:
        m = tomllib.loads(f.read_text(encoding="utf-8"))
        ov = mkindex.overrides_for(f)
        if isinstance(ov, str):
            print(f"{f.name}: {ov}"); problems += 1; continue
        m = {**m, **(ov or {})}
        if m.get("status") != "approved" or m.get("excluded"):
            continue
        url = m.get("url", "")
        if not url.startswith("https://"):
            print(f"{f.name}: url is not https://"); problems += 1; continue
        # The rule submit applies, enforced here too: a pull request can be
        # written by hand, so the tool that prepares one is no boundary.
        key = (m.get("id"), m.get("arch"), m.get("abi"))
        was = base_entries.get(key)
        if was and was[0] == m.get("version") and was[1] == m.get("revision", 0) and was[2] != m.get("sha256"):
            print(f"{f.name}: version {was[0]} revision {was[1]} is published with sha256 {was[2]}; "
                  f"different bytes need a new revision or version")
            problems += 1
            continue
        try:
            # Keyed by the whole URL: two uploads named release.zip must not
            # stand in for each other.
            dest = cache / hashlib.sha256(url.encode()).hexdigest()[:24] / url.rsplit("/", 1)[-1]
            if a.use_local_copies:          # tests: the bytes "at" the url
                data = (Path(a.use_local_copies) / url.rsplit("/", 1)[-1]).read_bytes()
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
            else:
                data = download(url, dest)
        except Exception as exc:
            print(f"{f.name}: cannot download {url}: {exc}")
            if a.review_all:
                # A pass recorded earlier must not outlive a file that is gone.
                listed.append(f.name)
                if safe_name(m):
                    rec = {"schema": 1, "suite": rv.SUITE, "id": m.get("id"), "arch": m.get("arch"),
                           "abi": m.get("abi"), "date": a.today or rv.utc_today(), "sha256": "", "size": 0,
                           "manifest_digest": rv.manifest_digest(m), "result": "failed",
                           "checks": [{"name": "download", "result": "failed", "detail": str(exc)}],
                           "unchecked": []}
                    (review_dir / (mkindex.variant_file(m)[:-5] + ".json")).write_text(
                        json.dumps(rec, indent=1) + "\n", encoding="utf-8")
            else: problems += 1
            continue
        # The review: an archive this check cannot read has not been checked,
        # and that fails, where the generator's local run would only warn.
        rec = rv.review(m, dest, data, today=a.today)
        if safe_name(m):
            out = review_dir / (mkindex.variant_file(m)[:-5] + ".json")
            out.write_text(json.dumps(rec, indent=1) + "\n", encoding="utf-8")
        bad = [c for c in rec["checks"] if c["result"] != "passed"]
        for c in bad:
            print(f"{f.name}: {c['name']}: {c['detail']}")
        if bad:
            if a.review_all: listed.append(f.name)
            else: problems += 1
            continue
        left = ", ".join(u["name"] for u in rec["unchecked"])
        print(f"{f.name}: reviewed (suite {rec['suite']}, {len(data)} bytes; not decidable from the files: {left})",
              flush=True)
    if listed:
        print(f"\n{len(listed)} published entr{'y stays' if len(listed) == 1 else 'ies stay'} Listed, "
              f"not Reviewed: {', '.join(listed)}")
    sys.stdout.flush()
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(a.out_dir) if a.out_dir else Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        # The trial starts from the published index.json, so its rule (keep
        # the packages it lists) is checked against the real file.
        prev = index_dir / "index.json"
        if prev.exists() and out.resolve() != index_dir:
            (out / "index.json").write_bytes(prev.read_bytes())
        # The layouts were checked above, against archives keyed by URL; the
        # generator's own lookup is by file name, so it is skipped.
        cmd = [sys.executable, str(HERE / "mkindex.py"), "--manifests", str(index_dir / "manifests"),
               "--out-dir", str(out), "--no-archive-check", "--review-dir", str(review_dir),
               "--runtime-dir", str(index_dir / "runtime-tests"), "--commit", a.commit]
        if (index_dir / "history.json").exists():
            cmd += ["--history", str(index_dir / "history.json")]
        elif a.publish:
            # The first publication with dates: the workflow derives them
            # from the repository's own history, so history.json too is
            # written by it and by nobody else.
            import catalogue_history as ch
            boot = work / "history.json"
            boot.write_text(ch.dump(ch.bootstrap(index_dir)), encoding="utf-8")
            print("history.json: derived from the git history of the published catalogue files", flush=True)
            cmd += ["--history", str(boot)]
        if a.today:
            cmd += ["--today", a.today]
        r = subprocess.run(cmd)
        if r.returncode:
            problems += 1
    if problems:
        print(f"\n{problems} problem(s)")
        sys.exit(1)
    print("\nall checks pass")


if __name__ == "__main__":
    main()
