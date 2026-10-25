#!/usr/bin/env python3
"""The examples in the documentation, checked against the real validator.

Every fenced ```toml or ```json block in docs/guide/ must
be preceded by a marker saying what it is, and is checked accordingly:

  <!-- validate: manifest -->     a complete index manifest: tools/mkindex.py
                                   must find nothing wrong with it, and, when
                                   the archive is in .cache/archives, subdir
                                   and icon must be in it
  <!-- validate: fragment -->     part of one: merged over the guide's
                                   manifest, the result must still pass
  <!-- validate: rejected: TEXT -->  must be refused, with TEXT in a reason
  <!-- validate: index-entry -->  must equal what mkindex.py generates from
                                   the guide's manifest
  <!-- validate: registry -->     must match the registry entry the client
                                   wrote in the documented walk
                                   (tests/fixtures/docs-walk-registry.json)
  <!-- validate: none -->         shown for reading only (output, layouts)

Every ```toml block in docs/rfc/ must be marked `<!-- proposal -->`, must
parse, and must NOT pass as a current manifest -- a proposal that the current
validator accepts could be mistaken for a supported format. An implemented
RFC may instead show a block with `<!-- validate: none -->`, for reading; the
checked example of an implemented field is the one in docs/guide/.
"""
import json, re, subprocess, sys, tempfile, tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE / "tools"))
import mkindex  # noqa: E402

MARK = re.compile(r"<!--\s*(validate:\s*(.*?)|proposal)\s*-->")
FENCE = re.compile(r"^```(\w*)\s*$")
fails, notes = [], []


def blocks(path):
    """(marker or None, language, text, line) for each fenced block."""
    lines = path.read_text(encoding="utf-8").splitlines()
    out, pending, i = [], None, 0
    while i < len(lines):
        m = MARK.search(lines[i])
        if m:
            pending = m.group(2) if m.group(2) is not None else "proposal"
            i += 1
            continue
        f = FENCE.match(lines[i])
        if f:
            start, lang, body = i + 1, f.group(1), []
            i += 1
            while i < len(lines) and not lines[i].startswith("```"):
                body.append(lines[i]); i += 1
            out.append((pending, lang, "\n".join(body) + "\n", start))
            pending = None
        elif lines[i].strip():
            pending = None if not lines[i].lstrip().startswith("<!--") else pending
        i += 1
    return out


def fail(where, msg):
    fails.append(f"{where}: {msg}")


def problems_of(m):
    probs = mkindex.check(m)
    if not probs:
        more, warn = mkindex.check_archive(m, HERE / ".cache/archives")
        probs += more
        notes.extend(warn)
    return probs


def generated_entry(manifest_text):
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "m").mkdir()
        (d / "m/example.toml").write_text(manifest_text)
        subprocess.run([sys.executable, str(HERE / "tools/mkindex.py"), "--manifests", str(d / "m"),
                        "--out-dir", str(d), "--cache", str(HERE / ".cache/archives")],
                       check=True, capture_output=True)
        pk = json.loads((d / "index-v2.json").read_text())["packages"]
        return pk[0] if pk else None


guide = HERE / "docs/guide/packaging.md"
base_text = next((t for mk, lang, t, _ in blocks(guide) if mk == "manifest"), None)
if base_text is None:
    sys.exit("docs/guide/packaging.md has no <!-- validate: manifest --> block")
base = tomllib.loads(base_text)

walk = HERE / "tests/fixtures/docs-walk-registry.json"
real_registry = json.loads(walk.read_text())

current = sorted((HERE / "docs/guide").glob("*.md"))
checked = 0
for path in current:
    rel = path.relative_to(HERE)
    for mk, lang, text, line in blocks(path):
        where = f"{rel}:{line}"
        if lang not in ("toml", "json"):
            continue
        if mk is None:
            fail(where, f"```{lang} block without a <!-- validate: ... --> marker"); continue
        checked += 1
        try:
            if mk == "none":
                continue
            if mk == "manifest":
                p = problems_of(tomllib.loads(text))
                if p: fail(where, "the validator refuses it: " + "; ".join(p))
            elif mk == "fragment":
                p = problems_of({**base, **tomllib.loads(text)})
                if p: fail(where, "merged over the guide's manifest, refused: " + "; ".join(p))
            elif mk.startswith("rejected:"):
                want = mk.split(":", 1)[1].strip()
                p = mkindex.check({**base, **tomllib.loads(text)})
                if not any(want in x for x in p):
                    fail(where, f"expected a rejection mentioning {want!r}, got {p}")
            elif mk == "index-entry":
                got, shown = generated_entry(base_text), json.loads(text)
                if got != shown:
                    diff = sorted(k for k in set(got or {}) | set(shown)
                                  if (got or {}).get(k) != shown.get(k))
                    fail(where, f"differs from mkindex.py output in {diff}")
            elif mk == "registry":
                shown = json.loads(text)
                if set(shown) != set(real_registry):
                    fail(where, f"keys differ from the walk's registry: "
                                f"{sorted(set(shown) ^ set(real_registry))}")
                for k, v in shown.items():
                    if k == "contents":
                        missing = [c for c in v if c not in real_registry["contents"]]
                        if missing: fail(where, f"contents not in the walk's registry: {missing}")
                    elif real_registry.get(k) != v:
                        fail(where, f"{k} differs from the walk's registry")
            else:
                fail(where, f"unknown marker {mk!r}")
        except (tomllib.TOMLDecodeError, json.JSONDecodeError) as exc:
            fail(where, f"does not parse: {exc}")

for path in sorted((HERE / "docs/rfc").glob("*.md")):
    rel = path.relative_to(HERE)
    for mk, lang, text, line in blocks(path):
        if lang != "toml":
            continue
        where = f"{rel}:{line}"
        if mk == "none":
            checked += 1
            continue
        if mk != "proposal":
            fail(where, "a TOML example in an RFC must be marked <!-- proposal --> "
                        "(or <!-- validate: none --> in an implemented RFC)"); continue
        checked += 1
        try:
            m = tomllib.loads(text)
        except tomllib.TOMLDecodeError as exc:
            fail(where, f"does not parse: {exc}"); continue
        if not mkindex.check(m):
            fail(where, "the current validator accepts this proposal as a manifest")

for n in sorted(set(notes)):
    print(f"note: {n}")
if fails:
    print("\n".join(fails))
    print(f"DOC EXAMPLES: {len(fails)} failed of {checked}")
    sys.exit(1)
print(f"DOC EXAMPLES: {checked} checked, all as documented")
