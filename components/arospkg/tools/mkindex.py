#!/usr/bin/env python3
"""Build arospkg-index/index-v2.json and index.json from APPROVED manifests.

index-v2.json holds every published package and is what clients after 0.3.2
fetch. index.json is the same data restricted to the packages it already
listed, within what clients up to 0.3.2 can read (OLD_CLIENT_MAXTOK).

The approved manifests and the index live in the arospkg-index repository,
beside this one; that is the only copy. Unreviewed candidates stay here, in
index/candidates/, and never reach it.

This is the publication half of the pipeline. It reads the manifests, not the
catalogue -- which is the whole point of the split. The earlier version of this
tool generated the index directly from the catalogue with `depends: []` hard
coded, so a dependency written into a manifest by hand could never reach the
index. It was a catalogue importer wearing the name of an index generator.

    catalogue  --import_catalogue.py-->  candidates.json + skeletons
    manifests  --mkindex.py---------->   index-v2.json, index.json

A manifest enters the index only if it is approved AND complete. "Complete"
is checked, not assumed:

  * status == "approved"
  * sha256 present and 64 hex characters
  * depends_checked == true -- an empty `depends` with depends_checked false
    means nobody has looked, which is not the same as having no dependencies,
    and shipping the first as the second is how a package manager installs
    something that cannot run. `depends_checked` asserts a deliberate review
    informed by tools/deps.py, not that a scan was run.
  * every id in `depends` resolves to another published manifest for the SAME
    architecture. Without this an approved manifest could name a package that
    does not exist and the index would publish it, so the failure would land on
    a user at install time instead of on us at generation time.

`requires_system` is separate from `depends` and is deliberately NOT closed
over: it names things the machine must already provide (a library that ships
with the OS, a device, a datatype). "No package in the index provides X" and
"this machine does not have X" are different conditions and get different
messages, so they are different fields.

Everything rejected is listed with its reason. A quiet index is a lying index.
"""

import argparse
import json
import re
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import catalogue as cat

SHA_RE = re.compile(r"^[0-9a-f]{64}$")
# Requirement types the client can actually decide. Anything else is published
# but will come back "undetermined" on every machine, so listing them here is
# how the host tool says which ones carry a real verdict.
REQ_TYPES = ("library",)
REQUIRED = ("id", "version", "arch", "url", "size", "sha256", "kind")
# The ABIs the 0.3 client knows. It refuses an entry with any other value,
# or with none, before downloading -- so publishing one only produces an
# entry nobody can install.
# The same list as pkg_abi_known() in src/libpkg/entries.c.
ABIS = ("v0", "v1", "v11")
# CPUs an entry may name: those the client is built for. Kept apart from
# catalogue.SUPPORTED_ARCH, which decides what the importer offers as
# candidates; widening that would import every i386 upload at once.
ARCHES = ("x86_64", "aarch64", "i386")

# What the 0.3 client can hold, from its fixed buffers (src/libpkg/pkg.h,
# entries.c, internal.h, req.h). Past these it truncates without a word.
# Fields the client ACTS on are refused when they do not fit: a cut url
# fetches another address, a cut subdir installs nothing. Fields it only
# shows are a note: a shortened summary is a display limit, not wrong data.
OPERATIONAL_MAX = {"id": 63, "version": 63, "arch": 15, "abi": 7, "url": 511,
                   "subdir": 127, "icon": 127}
DISPLAY_MAX = {"summary": 159, "category": 63, "kind": 15, "license": 63}
SOURCE_MAX = 255
REQ_MAX = {"id": 63, "type": 23}
# The client does not decode JSON escapes: a string reaches it exactly as it
# stands in the file. So every string must be one json.dumps writes without
# an escape -- printable ASCII other than '"' and '\' -- or the client would
# act on something other than what the manifest says (measured 2026-09-29:
# a '"' in version made every later upgrade compare unequal).
PLAIN = re.compile(r'^[\x20-\x21\x23-\x5b\x5d-\x7e]*$')
# What a client can read; every key and value is one JSON value.
# Clients up to 0.3.2 hold 8192 values in a fixed array and take 4 MiB, and
# they read the same published file, so an index past that breaks them with
# "the index is not valid JSON". That is their limit, not the format's.
OLD_CLIENT_MAXTOK = 8192
OLD_CLIENT_MAXBYTES = 4 * 1024 * 1024
# Later clients allocate what the file needs, up to these guards against a
# broken or hostile file (src/libpkg/json.h PKG_JSON_MAXTOK, ops.c INDEX_MAX).
CLIENT_MAXTOK = 1 << 20
CLIENT_MAXBYTES = 16 * 1024 * 1024
REVISION_MAX = 2**31 - 1
# post_install_notes: what the client holds (src/libpkg/pkg.h PKG_NOTES_*).
# Refused, not shortened, past these: a cut sentence can say the opposite.
NOTES_LINES, NOTES_LINE = 8, 159


# [launch]: how an installed application is started (docs/rfc/0002). The
# same rules as the client's reader (src/libpkg/app.c launch_from_object): a
# table the client would refuse must not be published, because it would
# turn Run into an error on every machine.
LAUNCH_KEYS = ("executable", "mode", "working_directory")
LAUNCH_MODES = ("workbench", "shell")
LAUNCH_PATH_MAX = 255
# The file the client reads inside the drawer, written by apkg-pack into an
# archive or by the client at installation (src/libpkg/app.h PKG_APP_FILE).
APP_FILE = "application.json"
APP_SCHEMA = 1

# index.json is for clients up to 0.3.2. It keeps the packages it already
# lists plus every build of the package manager itself, so a client without
# self-update can always find its way to a current apkg. It carries only the
# fields those clients read (src/libpkg/entries.c, ops.c, req.c at v0.3.2),
# and stays under OLD_CLIENT_BUDGET of what they can hold: the margin is for
# the entries it keeps growing (requirements, longer URLs), and running into
# it is a decision for a person, not a reason to cut entries silently.
OLD_CLIENT_FIELDS = ("id", "version", "revision", "summary", "category", "arch", "abi", "kind",
                     "url", "size", "sha256", "subdir", "icon", "requires_system", "source", "license")
OLD_CLIENT_ALWAYS = ("arospkg",)
OLD_CLIENT_BUDGET = 0.80


# Filled in from the uploaded file when a package is submitted, never written
# by its author: what the author's copy says could only disagree with the file.
PUBLICATION_FIELDS = ("url", "size", "sha256", "status")


def check(m, stage="index"):
    """Return a list of reasons this manifest cannot be published.

    stage "index": a manifest in arospkg-index, complete and approved.
    stage "author": the manifest an author ships in the archive
    (.arospkg/manifest.toml); the same rules, without the publication fields,
    which it must not carry."""
    problems = []
    if stage == "author":
        for k in PUBLICATION_FIELDS:
            if k in m:
                problems.append(f"{k}: set when the archive is submitted, from the uploaded file; "
                                "remove it from the manifest")
        m = {**m, "status": "approved", "url": "https://placeholder.invalid/p.zip",
             "size": 1, "sha256": "0" * 64}
    # A deliberate exclusion is not the same as an unreviewed skeleton, and
    # collapsing the two would hide a decision inside a backlog count.
    if m.get("excluded"):
        return [f'excluded: {m["excluded"]}']
    if m.get("status") != "approved":
        problems.append(f'status is {m.get("status", "missing")!r}, not "approved"')
    for k in REQUIRED:
        if m.get(k) in (None, ""):
            problems.append(f"missing {k}")
    sha = m.get("sha256", "")
    if sha and not SHA_RE.match(sha):
        problems.append("sha256 is not 64 hex characters")
    if not m.get("depends_checked", False):
        problems.append("depends_checked is false (not investigated)")
    if m.get("arch") not in ARCHES:
        problems.append(f'arch {m.get("arch")!r} is not one we build for')
    if m.get("id") and not cat.ID_RE.match(str(m["id"])):
        problems.append(f'id {m["id"]!r} is not a legal package id')
    if m.get("abi") not in ABIS:
        problems.append(f'abi {m.get("abi")!r} is not one the client knows '
                        f'({", ".join(ABIS)}); it would refuse the entry')
    url = m.get("url")
    if isinstance(url, str) and url and not url.startswith("https://"):
        problems.append("url is not https://; the client fetches nothing else")
    size = m.get("size")
    if size not in (None, "") and (isinstance(size, bool) or not isinstance(size, int) or size <= 0):
        problems.append(f"size {size!r} is not a positive integer")
    if "revision" in m:
        r = m["revision"]
        if isinstance(r, bool) or not isinstance(r, int) or not 1 <= r <= REVISION_MAX:
            problems.append(f"revision {r!r} is not an integer from 1 to {REVISION_MAX}")
    problems += check_strings(m)
    problems += check_relations(m)
    problems += check_requires(m.get("requires_system", []))
    problems += check_notes(m.get("post_install_notes"))
    problems += check_launch(m.get("launch"))
    return problems


# Relations between packages (docs/guide/metadata.md, "Relations"). The
# same rules as the client's reader (src/libpkg/relations.c): a declaration
# the client would refuse must not be published.
REL_MAX = 16
REL_KEYS = ("depends", "recommends", "conflicts", "replaces")
# What a catalogue entry needs from a client: a client that does not know
# a feature named here must not install the entry. Clients before 0.6 do not
# read depends, conflicts or replaces at all, so such entries go only to
# index-v3.json, which they never fetch.
#
# "large-members": a member over 64 MiB unpacked, which clients before 0.6
# refused ("member over size limit"). Known from the entry's review record
# (tools/review.py, suite 3 on), so an entry reviewed before that has none.
CLIENT_FEATURES = ("relations", "large-members")
OLD_CLIENT_MAX_MEMBER = 64 * 1024 * 1024
DEPEND_RE = re.compile(r"^([a-z][a-z0-9._-]{0,63})(?: >= ([0-9]{1,9}(?:\.[0-9]{1,9}){0,7}))?$")


def numeric_version(v):
    """The client's ordering (upgrade.c version_compare): plain dotted
    numbers only. A tuple, or None when the version cannot be ordered."""
    if not isinstance(v, str) or not re.match(r"^[0-9]{1,9}(\.[0-9]{1,9}){0,7}$", v):
        return None
    parts = [int(x) for x in v.split(".")]
    return tuple(parts + [0] * (8 - len(parts)))


def parse_depend(s):
    """(id, min_version or None), or None when the string is not one."""
    hit = DEPEND_RE.match(s) if isinstance(s, str) else None
    return (hit.group(1), hit.group(2)) if hit else None


def check_relations(m):
    """depends, recommends, conflicts and replaces: their form, and the
    combinations that cannot mean anything."""
    problems = []
    own = str(m.get("id", "")).lower()
    named = {}
    dep = m.get("depends", [])
    if not isinstance(dep, list):
        return ["depends must be a list"]
    for i, d in enumerate(dep):
        got = parse_depend(d)
        if not got:
            problems.append(f"depends[{i}]: {d!r} is not \"id\" or \"id >= version\" with a version "
                            "of dotted numbers (the only versions apkg can order)")
            continue
        named.setdefault(got[0], []).append("depends")
    rec = m.get("recommends", [])
    if not isinstance(rec, list) or not all(isinstance(r, dict) for r in rec):
        problems.append("recommends: write one [[recommends]] table per package"); rec = []
    for i, r in enumerate(rec):
        extra = set(r) - {"id", "min_version", "reason"}
        if extra:
            problems.append(f"recommends[{i}]: unknown key(s) {', '.join(sorted(extra))}")
        if not cat.ID_RE.match(str(r.get("id", ""))):
            problems.append(f"recommends[{i}]: id {r.get('id')!r} is not a legal package id")
        else:
            named.setdefault(r["id"], []).append("recommends")
        if "min_version" in r and numeric_version(r["min_version"]) is None:
            problems.append(f"recommends[{i}]: min_version {r['min_version']!r} is not dotted numbers")
        why = r.get("reason")
        if not isinstance(why, str) or not why.strip():
            problems.append(f"recommends[{i}]: reason missing; say what it adds")
        elif not PLAIN.match(why) or len(why) > NOTES_LINE:
            problems.append(f"recommends[{i}]: reason must be plain ASCII of at most {NOTES_LINE} characters")
    for k in ("conflicts", "replaces"):
        v = m.get(k, [])
        if not isinstance(v, list):
            problems.append(f"{k} must be a list of package ids"); continue
        for i, x in enumerate(v):
            if not isinstance(x, str) or not cat.ID_RE.match(x):
                problems.append(f"{k}[{i}]: {x!r} is not a legal package id")
            else:
                named.setdefault(x, []).append(k)
    for k in REL_KEYS:
        v = m.get(k, [])
        if isinstance(v, list) and len(v) > REL_MAX:
            problems.append(f"{k}: {len(v)} entries; the client reads at most {REL_MAX}")
    # What the client keeps: ids and versions of at most 63 characters.
    for i, d in enumerate(dep):
        got = parse_depend(d)
        if got and (len(got[0]) > 63 or (got[1] and len(got[1]) > 63)):
            problems.append(f"depends[{i}]: id and version are at most 63 characters each")
    for i, r in enumerate(rec):
        if len(str(r.get("id", ""))) > 63 or len(str(r.get("min_version", ""))) > 63:
            problems.append(f"recommends[{i}]: id and min_version are at most 63 characters each")
    for k in ("conflicts", "replaces"):
        for i, x in enumerate(m.get(k, []) if isinstance(m.get(k, []), list) else []):
            if isinstance(x, str) and len(x) > 63:
                problems.append(f"{k}[{i}]: at most 63 characters")
    for x, where in sorted(named.items()):
        if x == own:
            problems.append(f"{', '.join(sorted(set(where)))}: names the package itself ({x})")
        if len(where) != len(set(where)):
            problems.append(f"{x} is named twice in {where[0]}")
        # replaces implies a conflict, so naming a package in both is
        # redundant but consistent; every other pair contradicts itself.
        if len(set(where)) > 1 and set(where) != {"conflicts", "replaces"}:
            problems.append(f"{x} is in {' and '.join(sorted(set(where)))}: a package cannot be "
                            "both needed and refused, or both needed and replaced")
    return problems


def client_features(m, review_rec=None):
    """The client features an entry needs (CLIENT_FEATURES). `review_rec`: its
    review record when one applies to these bytes (review_record())."""
    out = ["relations"] if any(m.get(k) for k in ("depends", "conflicts", "replaces")) else []
    if review_rec and (review_rec.get("sizes") or {}).get("largest_member", 0) > OLD_CLIENT_MAX_MEMBER:
        out.append("large-members")
    return out


def launch_path_problem(rel):
    """RFC 0002 section 6 for one relative path; None when acceptable.
    The same rules as app_path_ok() in src/libpkg/app.c."""
    if not isinstance(rel, str) or not rel:
        return "empty or not a string"
    if len(rel) > LAUNCH_PATH_MAX:
        return f"longer than {LAUNCH_PATH_MAX} characters"
    if rel.startswith("/"):
        return f"{rel!r} is absolute; give it relative to the drawer"
    if ":" in rel:
        return f"{rel!r} names a volume (':'); give it relative to the drawer"
    if "\\" in rel:
        return f"{rel!r} uses '\\'; AROS paths use '/'"
    if not PLAIN.match(rel):
        return f"{rel!r} has a character the client cannot carry (control, '\"' or non-ASCII)"
    for part in rel.split("/"):
        if part == "":
            return f"{rel!r} has an empty component"
        if part == "..":
            return f"{rel!r} leads out of the drawer ('..')"
        if part == "." and rel != ".":
            return f"{rel!r} has a '.' component; write the path without it"
    return None


def launch_normal(launch):
    """The table as the client reads it: working_directory filled in."""
    exe = launch["executable"]
    wd = launch.get("working_directory", exe.rsplit("/", 1)[0] if "/" in exe else ".")
    return {"executable": exe, "mode": launch["mode"], "working_directory": wd}


def check_launch(launch):
    """[launch], against what the client accepts."""
    if launch is None:
        return []
    if not isinstance(launch, dict):
        return ["launch: not a table; write [launch] with executable and mode"]
    problems = []
    extra = set(launch) - set(LAUNCH_KEYS)
    if extra:
        problems.append(f"launch: unknown key(s) {', '.join(sorted(extra))}; the client refuses a table "
                        f"it does not fully understand (known: {', '.join(LAUNCH_KEYS)})")
    exe = launch.get("executable")
    why = launch_path_problem(exe)
    if why:
        problems.append(f"launch.executable: {why}")
    elif exe == "." or exe.endswith(".info"):
        problems.append(f"launch.executable: {exe!r} is not a program; name the program file "
                        "(its icon stays beside it)")
    if launch.get("mode") not in LAUNCH_MODES:
        problems.append(f"launch.mode: {launch.get('mode')!r}; use \"workbench\" (as from its icon) "
                        "or \"shell\" (as a Shell command)")
    if "working_directory" in launch:
        why = launch_path_problem(launch["working_directory"])
        if why:
            problems.append(f"launch.working_directory: {why}")
    if not problems and launch["mode"] == "workbench":
        own = exe.rsplit("/", 1)[0] if "/" in exe else "."
        if launch.get("working_directory", own) != own:
            problems.append(f"launch.working_directory: a Workbench start runs in the program's own "
                            f"drawer ({own!r}); leave working_directory out, or use mode = \"shell\"")
    return problems


def app_description(m):
    """application.json for a manifest with [launch], as the client writes it
    (src/libpkg/app.c app_text), so a file apkg-pack puts in an archive and
    one the client writes at installation read the same."""
    out = {"schema": APP_SCHEMA, "id": m["id"], "version": m["version"]}
    if m.get("revision"):
        out["revision"] = int(m["revision"])
    out["targets"] = [{"os": "aros", "arch": m["arch"], "abi": m.get("abi") or ""}]
    out["requires_system"] = [{k: r[k] for k in ("type", "id", "min_version") if r.get(k)}
                              for r in m.get("requires_system", [])]
    out["launch"] = launch_normal(m["launch"])
    return out


def app_description_problems(m, data, where):
    """An application.json shipped in an archive must say what the manifest
    says: the catalogue and the drawer would otherwise start it differently."""
    try:
        got = json.loads(data)
    except Exception as exc:
        return [f"{where}: not valid JSON ({exc}); let apkg-pack write it"]
    if "launch" not in m:
        return [f"{where}: the archive says how to start it, the manifest has no [launch]; "
                "add the same [launch] to the manifest"]
    want = app_description(m)
    bad = [k for k in ("schema", "id", "version", "revision", "targets", "launch")
           if got.get(k) != want.get(k)]
    if bad:
        return [f"{where}: differs from the manifest in {', '.join(bad)}; rebuild with apkg-pack"]
    return []


def check_notes(v):
    """post_install_notes: optional plain lines a user reads after installing."""
    if v is None:
        return []
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
        return ["post_install_notes: not a list of strings; write one string per line"]
    problems = []
    if len(v) > NOTES_LINES:
        problems.append(f"post_install_notes: {len(v)} lines; maximum is {NOTES_LINES}")
    # Counted from 0, as requires_system[i] is.
    for i, line in enumerate(v):
        if not line.strip():
            problems.append(f"post_install_notes[{i}]: empty; remove the line")
        elif not PLAIN.match(line):
            problems.append(f"post_install_notes[{i}]: has a character the client would read "
                            "differently; use printable ASCII without '\"' or '\\'")
        elif len(line) > NOTES_LINE:
            problems.append(f"post_install_notes[{i}]: {len(line)} characters; maximum is {NOTES_LINE}")
    return problems


def check_strings(m):
    """Every string the index carries, against what the 0.3 client can read."""
    problems = []
    fields = [(k, m.get(k)) for k in list(OPERATIONAL_MAX) + list(DISPLAY_MAX)]
    src = m.get("source")
    if isinstance(src, dict):
        fields.append(("source.repository", src.get("repository")))
    for k, v in fields:
        if v is None or v == "":
            continue
        if not isinstance(v, str):
            problems.append(f"{k} is not a string")
        elif not PLAIN.match(v):
            problems.append(f"{k} {v!r} has a character the 0.3 client would read "
                            "differently: use printable ASCII without '\"' or '\\'")
        else:
            limit = OPERATIONAL_MAX.get(k)
            if limit and len(v) > limit:
                problems.append(f"{k} is {len(v)} characters; the client keeps {limit}")
    for k, limit in DISPLAY_MAX.items():
        v = m.get(k)
        if isinstance(v, str) and len(v) > limit:
            print(f"  note: {m.get('id')}: {k} is {len(v)} characters; the client shows {limit}")
    if isinstance(src, dict) and isinstance(src.get("repository"), str) \
            and len(src["repository"]) > SOURCE_MAX:
        print(f"  note: {m.get('id')}: source.repository is longer than the {SOURCE_MAX} the client shows")
    for k in ("installs_on", "runs_on", "does_not_run_on"):
        v = m.get(k, [])
        if not isinstance(v, list) or not all(isinstance(x, str) and PLAIN.match(x) for x in v):
            problems.append(f"{k} must be a list of plain ASCII strings")
    return problems


def check_requires(reqs):
    """requires_system must have a shape the client can read.

    The client refuses a malformed entry rather than skipping it -- a
    requirement silently dropped is indistinguishable from a package with no
    requirements. That refusal would land on a user at install time, so the
    same check runs here, where it lands on us at generation time instead.

    One TOML trap is worth knowing about: every key after a `[[requires_system]]`
    header belongs to that table, so the scalar keys must come BEFORE the table
    arrays in a manifest. Putting `kind` after them silently moved it inside a
    requirement and this tool reported `missing kind`.
    """
    problems = []
    if not isinstance(reqs, list):
        return ["requires_system is not a list"]
    for i, r in enumerate(reqs):
        if not isinstance(r, dict):
            problems.append(f"requires_system[{i}] is not a table")
            continue
        for k, limit in REQ_MAX.items():
            v = r.get(k)
            if isinstance(v, str) and v and (not PLAIN.match(v) or len(v) > limit):
                problems.append(f"requires_system[{i}] {k} {v!r} is not plain ASCII of "
                                f"at most {limit} characters")
        if not r.get("id"):
            problems.append(f"requires_system[{i}] has no id")
        if not r.get("type"):
            problems.append(f"requires_system[{i}] has no type")
        elif r["type"] not in REQ_TYPES:
            # Not a rejection: an honest "undetermined" is a legitimate
            # outcome, and refusing the manifest would mean only requirements
            # we can check may be declared, which is backwards.
            print(f'  note: requires_system[{i}] type {r["type"]!r} cannot be '
                  f"checked by the client; it will report undetermined")
        mv = r.get("min_version", 0)
        if isinstance(mv, bool) or not isinstance(mv, int) or mv < 0:
            problems.append(f"requires_system[{i}] min_version {mv!r} is not a "
                            "non-negative integer")
        extra = set(r) - {"id", "type", "min_version"}
        if extra:
            problems.append(f"requires_system[{i}] has unknown key(s) "
                            f"{', '.join(sorted(extra))}")
    return problems


def archive_names(path):
    """Member names in a ZIP or an LHA, as a set."""
    return set(archive_list(path))


def archive_list(path):
    """Member names in a ZIP or an LHA, in archive order, duplicates kept:
    the review needs to see a name stored twice, which a set hides.

    The client picks its reader from the archive's first bytes, and since it
    reads LHA this tool has to as well: handing an LHA to zipfile raised
    BadZipFile, which would have crashed the generator on the first LHA
    manifest rather than checking it. LHA listing shells out to `lha`, which
    is not part of the client and is only ever used here, on the host.
    """
    import subprocess
    import zipfile
    head = path.open("rb").read(8)
    if head[:4] == b"PK\x03\x04":
        return zipfile.ZipFile(path).namelist()
    if head[2:3] == b"-" and head[6:7] == b"-":
        out = subprocess.run(["lha", "lq", str(path)], capture_output=True,
                             text=True, check=True).stdout
        # lhasa has no names-only listing, so the name is whatever follows the
        # timestamp column -- `Sep 20 09:24` for this year, `Sep 20  2024`
        # otherwise. Anchoring on that keeps names containing spaces intact,
        # which splitting on whitespace would not.
        stamp = re.compile(r" [A-Z][a-z]{2} +\d{1,2} +(?:\d{2}:\d{2}|\d{4}) (.+)$")
        names = []
        for ln in out.splitlines():
            hit = stamp.search(ln)
            if hit:
                # A link is listed as "name -> target"; the member is the name.
                name = hit.group(1)
                names.append(name.split(" -> ", 1)[0] if ln.startswith("l") else name)
        if not names:
            raise ValueError("lha listed no members")
        return names
    raise ValueError("not a ZIP or LHA archive")


def archive_links(path):
    """Members stored as symbolic links. The client installs none, and a
    link can point anywhere, so the review refuses an archive that has one."""
    import stat
    import subprocess
    import zipfile
    if path.open("rb").read(4) == b"PK\x03\x04":
        return [i.filename for i in zipfile.ZipFile(path).infolist()
                if stat.S_ISLNK(i.external_attr >> 16)]
    out = subprocess.run(["lha", "lq", str(path)], capture_output=True, text=True, check=True).stdout
    return [ln.split(" -> ", 1)[0].rsplit(" ", 1)[-1] for ln in out.splitlines()
            if ln.startswith("l") or " -> " in ln]


# What the client refuses in an archive member (src/libpkg/ops.c), and its
# limits on what it installs.
MEMBER_MAX, PKGPATH_MAX, FILES_MAX = 511, 255, 2048


# Fields an override may not change: they identify the entry or come from the
# uploaded file.
OVERRIDE_FIXED = ("id", "arch", "abi", "url", "size", "sha256")


def overrides_for(manifest_file):
    """overrides/<same file name> beside manifests/: a dict, None when there
    is none, or a string saying why it cannot be used."""
    p = manifest_file.parent.parent / "overrides" / manifest_file.name
    if not p.is_file():
        return None
    try:
        ov = tomllib.loads(p.read_text(encoding="utf-8"))
    except Exception as exc:
        return f"overrides/{p.name}: unparseable: {exc}"
    bad = sorted(set(ov) & set(OVERRIDE_FIXED))
    if bad:
        return f"overrides/{p.name}: may not change {', '.join(bad)}"
    return ov


def variant_file(m):
    """The manifest file name of a variant: <id>.<arch>.<abi>.toml. Files named
    <id>.<arch>.toml, from before the ABI was part of the name, are still read."""
    return f"{m['id']}.{m['arch']}.{m['abi']}.toml"


def check_members(m, names, where="the archive"):
    """The member names of an archive, or of a drawer about to be packed,
    against the client's rules and the manifest's subdir and icon."""
    problems = []
    sub = m.get("subdir") or ""
    files = 0
    for n in sorted(names):
        rel = n[len(sub) + 1:] if sub and n.startswith(sub + "/") else (n if not sub else None)
        parts = n.rstrip("/").split("/")
        if n.startswith(("/", "\\")) or ":" in n or ".." in parts \
                or any(ord(ch) < 0x20 or ord(ch) == 0x7f for ch in n):
            problems.append(f"member {n[:60]!r}: the client refuses this name (absolute, a volume, "
                            "'..' or a control character); rename it")
        elif len(n.encode()) > MEMBER_MAX:
            problems.append(f"member {n[:60]}...: longer than the {MEMBER_MAX} bytes the client reads")
        elif rel is not None and len(rel.rstrip("/").encode()) > PKGPATH_MAX:
            problems.append(f"member {rel[:60]}...: path inside the package longer than {PKGPATH_MAX} bytes")
        if rel is not None and not n.endswith("/"):
            files += 1
    launch = m.get("launch")
    if isinstance(launch, dict) and (sub + "/" if sub else "") + APP_FILE not in names:
        files += 1          # the client writes application.json into the drawer
    if files > FILES_MAX:
        problems.append(f"subdir: {files} files; the client installs at most {FILES_MAX}")
    if m.get("icon") and m["icon"] not in names:
        problems.append(f'icon: {m["icon"]!r} is not at that path in {where}')
    launch = m.get("launch")
    if isinstance(launch, dict) and not check_launch(launch):
        prefix = sub + "/" if sub else ""
        exe = prefix + launch["executable"]
        if exe not in names:
            problems.append(f"launch.executable: {launch['executable']!r} is not in {where} "
                            f"(looked for {exe})")
        elif launch["mode"] == "workbench" and exe + ".info" not in names:
            problems.append(f"launch: a Workbench start needs the program's icon, "
                            f"{launch['executable']}.info, beside it in {where}; without one Workbench "
                            "starts it as a Shell command with the default stack")
        wd = launch_normal(launch)["working_directory"]
        if wd != "." and prefix + wd + "/" not in names and not any(n.startswith(prefix + wd + "/") for n in names):
            problems.append(f"launch.working_directory: {wd!r} is not a drawer in {where}")
    if sub and not any(n.startswith(sub + "/") for n in names):
        problems.append(f'subdir: {sub!r} is not in {where}')
    return problems


def check_archive(m, cache):
    """subdir and icon must exist in the archive. Checked here, where it lands on
    us, because the client refuses an icon the archive lacks -- which is right,
    and which a wrong path in a published manifest would turn into a package
    nobody can install. `sdllopan` shipped with the icon INSIDE its drawer named
    as the drawer icon; this is the check that would have caught it."""
    name = m.get("url", "").rsplit("/", 1)[-1]
    hits = list(Path(cache).rglob(name)) if name else []
    if not hits:
        return [], [f"archive {name} not in {cache}: subdir and icon unchecked"]
    try:
        names = archive_names(hits[0])
    except Exception as exc:
        return [], [f"{name}: cannot list ({exc}): subdir and icon unchecked"]
    problems = check_members(m, names)
    app = (m["subdir"] + "/" if m.get("subdir") else "") + APP_FILE
    if app in names:
        problems += app_description_problems(m, archive_member(hits[0], app), f"{name}:{app}")
    return problems, []


def archive_member(path, member):
    """One member's bytes, from a ZIP or (through lhasa) an LHA."""
    import subprocess
    import zipfile
    if path.open("rb").read(4) == b"PK\x03\x04":
        return zipfile.ZipFile(path).read(member)
    return subprocess.run(["lha", "pq", str(path), member], capture_output=True, check=True).stdout


# Runtime tests (runtime-tests/<id>.<arch>.<abi>.toml in arospkg-index): a
# maintainer's record that a build was installed and started on a named
# system. Each names the archive bytes it was made with; the catalogue
# carries only the records for the bytes it currently offers, so a changed
# archive never inherits a test.
TEST_KEYS = ("sha256", "date", "system", "arch", "abi", "machine", "result", "checked", "note")
TEST_RESULTS = ("passed", "failed")
TEST_STEPS = ("install", "start", "run", "remove", "upgrade", "rollback", "verify")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def check_runtime_tests(tests, m):
    """Problems with a runtime-tests file's [[test]] list for manifest m."""
    if not isinstance(tests, list):
        return ["test: not a list of [[test]] tables"]
    problems = []
    for i, t in enumerate(tests):
        w = f"test[{i}]"
        if not isinstance(t, dict):
            problems.append(f"{w}: not a table"); continue
        extra = set(t) - set(TEST_KEYS)
        if extra:
            problems.append(f"{w}: unknown key(s) {', '.join(sorted(extra))}")
        for k in ("sha256", "date", "system", "arch", "abi", "machine", "result"):
            if not isinstance(t.get(k), str) or not t[k]:
                problems.append(f"{w}: {k} missing")
        if isinstance(t.get("sha256"), str) and not SHA_RE.match(t["sha256"]):
            problems.append(f"{w}: sha256 is not 64 hex characters")
        if isinstance(t.get("date"), str) and not DATE_RE.match(t["date"]):
            problems.append(f"{w}: date {t['date']!r} is not YYYY-MM-DD")
        if t.get("result") not in TEST_RESULTS:
            problems.append(f"{w}: result {t.get('result')!r}; use {' or '.join(TEST_RESULTS)}")
        if (t.get("arch"), t.get("abi")) != (m.get("arch"), m.get("abi")):
            problems.append(f"{w}: {t.get('arch')}/{t.get('abi')} is not this entry's {m.get('arch')}/{m.get('abi')}")
        steps = t.get("checked", [])
        if not isinstance(steps, list) or not steps or any(s not in TEST_STEPS for s in steps):
            problems.append(f"{w}: checked must list steps from {', '.join(TEST_STEPS)}")
        for k in ("system", "machine", "note"):
            v = t.get(k)
            if isinstance(v, str) and (not PLAIN.match(v) or len(v) > 159):
                problems.append(f"{w}: {k} is not plain ASCII of at most 159 characters")
    return problems


def runtime_tests_for(runtime_dir, m):
    """(tests for the current bytes, problems) from runtime_dir."""
    if not runtime_dir:
        return [], []
    p = Path(runtime_dir) / variant_file(m)
    if not p.is_file():
        return [], []
    try:
        tests = tomllib.loads(p.read_text(encoding="utf-8")).get("test", [])
    except Exception as exc:
        return [], [f"runtime-tests/{p.name}: unparseable: {exc}"]
    problems = [f"runtime-tests/{p.name}: {x}" for x in check_runtime_tests(tests, m)]
    if problems:
        return [], problems
    current = [{k: t[k] for k in ("system", "arch", "abi", "machine", "date", "result", "checked", "note") if k in t}
               for t in tests if t["sha256"] == m["sha256"]]
    return sorted(current, key=lambda t: t["date"], reverse=True), []


def review_record(review_dir, m):
    """The stored review record that applies to m's bytes and metadata, or None."""
    import review as rv
    if not review_dir:
        return None
    p = Path(review_dir) / (variant_file(m)[:-5] + ".json")
    try:
        rec = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return rec if rv.record_applies(rec, m) else None


def review_for(review_dir, m):
    """The catalogue's review object for m: reviewed only when a stored
    record of the automatic suite applies to these bytes and this metadata."""
    out = {"status": "listed"}
    rec = review_record(review_dir, m)
    if rec is None:
        return out
    return {"status": "reviewed", "suite": rec["suite"], "date": rec["date"],
            "checked": [c["name"] for c in rec.get("checks", [])],
            "unchecked": [u["name"] for u in rec.get("unchecked", [])]}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifests", default="../arospkg-index/manifests")
    ap.add_argument("--out-dir", default="../arospkg-index",
                    help="writes index-v2.json (every package) and index.json "
                         "(for clients up to 0.3.2) here")
    ap.add_argument("--out", help=argparse.SUPPRESS)
    ap.add_argument("--no-archive-check", action="store_true",
                    help="skip subdir/icon against cached archives (ci_check_index.py checks them itself)")
    ap.add_argument("--verbose", action="store_true", help="list every rejection")
    ap.add_argument("--cache", default=".cache/archives",
                    help="downloaded archives, to check subdir and icon against")
    ap.add_argument("--history", help="history.json: first publication dates (catalogue_history.py); "
                    "new variants are added with --today and written to OUT_DIR/history.json")
    ap.add_argument("--review-dir", help="review records (review.py), one <id>.<arch>.<abi>.json each")
    ap.add_argument("--runtime-dir", help="runtime-tests/<id>.<arch>.<abi>.toml: maintainer test records")
    ap.add_argument("--today", help="the publication date for new variants (default: today, UTC)")
    ap.add_argument("--commit", default="", help="the commit being published, recorded in history.json")
    args = ap.parse_args()
    if args.out:
        sys.exit("--out is gone: use --out-dir DIR, which gets index-v2.json and index.json")

    files = sorted(Path(args.manifests).glob("*.toml"))
    published, rejected, seen = [], [], {}
    # Manifests marked approved that did not make it into the index. Leaving
    # out a skeleton or an exclusion is the normal work of this tool; leaving
    # out something a person approved means the index would be incomplete,
    # and then the previous index is kept rather than overwritten.
    failed = set()

    for f in files:
        try:
            m = tomllib.loads(f.read_text(encoding="utf-8"))
        except Exception as exc:
            rejected.append((f.name, [f"unparseable: {exc}"]))
            if re.search(r'(?m)^\s*status\s*=\s*"approved"', f.read_text(encoding="utf-8", errors="replace")):
                failed.add(f.name)
            continue
        # A maintainer's deliberate correction to an author's entry, kept apart
        # so the author's next submission does not erase it, and applied here,
        # where every catalogue file is made.
        ov = overrides_for(f)
        if ov is not None:
            if isinstance(ov, str):
                rejected.append((f.name, [ov])); failed.add(f.name); continue
            m = {**m, **ov}
        approved = m.get("status") == "approved" and not m.get("excluded")

        problems = check(m)
        if not problems and not args.no_archive_check:
            more, warn = check_archive(m, args.cache)
            problems += more
            for w in warn:
                print(f"  warning: {f.name}: {w}")
        if problems:
            rejected.append((f.name, problems))
            if approved: failed.add(f.name)
            continue

        # A variant is its id, CPU and ABI: x86_64/v1 and x86_64/v11 builds of
        # one program are two entries, and the client picks its own.
        tests, more = runtime_tests_for(args.runtime_dir, m)
        if more:
            rejected.append((f.name, more))
            if approved: failed.add(f.name)
            continue

        key = (m["id"].lower(), m["arch"], m["abi"])
        if key in seen:
            rejected.append((f.name, [f"duplicate (id, arch, abi) with {seen[key]}"]))
            failed.add(f.name)
            continue
        seen[key] = f.name

        published.append({
            "id": m["id"], "version": m["version"], "summary": m.get("summary", ""),
            "arch": m["arch"], "abi": m.get("abi") or None,
            "category": m.get("category", ""), "url": m["url"], "size": m["size"],
            "sha256": m["sha256"], "depends": m.get("depends", []),
            # Relations (docs/guide/metadata.md); clients before 0.6 read none
            # of them, so an entry that needs them names the client feature
            # and is left out of index-v2.json.
            **({"recommends": [{k: r[k] for k in ("id", "min_version", "reason") if k in r}
                               for r in m["recommends"]]} if m.get("recommends") else {}),
            **({"conflicts": m["conflicts"]} if m.get("conflicts") else {}),
            **({"replaces": m["replaces"]} if m.get("replaces") else {}),
            **({"client_features": feats} if (feats := client_features(m, review_record(args.review_dir, m))) else {}),
            "requires_system": m.get("requires_system", []),
            # Carried through so a client can refuse a package on a system
            # where it is known not to run. Installing correctly and running
            # are different claims and the index must not conflate them.
            "installs_on": m.get("installs_on", []),
            "runs_on": m.get("runs_on", []),
            "does_not_run_on": m.get("does_not_run_on", []),
            "kind": m["kind"], "subdir": m.get("subdir", ""),
            "icon": m.get("icon", ""),
            "install": m.get("install", {}),
            # This port's release counter (docs/guide/metadata.md): absent means
            # the first packaging, and the client shows nothing for it. It was
            # never emitted before openloco stated one, so every upgrade test
            # ran on hand-written indexes.
            **({"revision": int(m["revision"])} if m.get("revision") else {}),
            # Where the source is, and under what licence, when the manifest
            # says: what `apkg show` prints. Older clients skip unknown keys.
            **({"source": m["source"]["repository"]}
               if isinstance(m.get("source"), dict) and m["source"].get("repository") else {}),
            **({"license": m["license"]} if m.get("license") else {}),
            # Shown after installing, by show and in the window. Older
            # clients skip unknown keys (tests/fixtures: checked with 0.3.2).
            **({"post_install_notes": m["post_install_notes"]} if m.get("post_install_notes") else {}),
            # How it is started (RFC 0002); clients before 0.5 skip it.
            **({"launch": launch_normal(m["launch"])} if m.get("launch") else {}),
            # Listed or Reviewed (tools/review.py), and the runtime tests made
            # with these very bytes. Clients before 0.6 skip both.
            "review": review_for(args.review_dir, m),
            "runtime_tests": tests,
        })

    # Closure. An approved manifest may name a dependency that does not exist;
    # publishing it would move the failure from us to a user at install time.
    # Iterated to a fixed point: dropping a package can orphan its dependents.
    # A dependency is the same CPU AND the same ABI: an x86_64/v1 build
    # cannot satisfy an x86_64/v11 one. A stated minimum version must be met
    # by what is published.
    while True:
      changed_any = False
      while True:
          have = {(p["id"].lower(), p["arch"], p["abi"]): p["version"] for p in published}
          broken = []
          for p in published:
              missing = []
              for d in p["depends"]:
                  did, dmin = parse_depend(d)
                  v = have.get((did, p["arch"], p["abi"]))
                  if v is None:
                      missing.append(f"{did} (not in the index for {p['arch']}/{p['abi']})")
                  elif dmin and (numeric_version(v) is None or numeric_version(v) < numeric_version(dmin)):
                      missing.append(f"{did} >= {dmin} (the index has {v})")
              if missing:
                  broken.append((p, missing))
          if not broken:
              break
          for p, missing in broken:
              published.remove(p)
              failed.add(f'{p["id"]}.{p["arch"]}.toml')
              rejected.append((f'{p["id"]}.{p["arch"]}.toml', [f"depends on {', '.join(missing)}"]))
      # A cycle has no order to install in; the client refuses one, so it is
      # refused here, where it lands on us.
      graph = {(p["id"].lower(), p["arch"], p["abi"]): [parse_depend(d)[0] for d in p["depends"]] for p in published}
      for p in list(published):
          start = (p["id"].lower(), p["arch"], p["abi"])
          seen, todo = set(), list(graph[start])
          while todo:
              x = todo.pop()
              if x == start[0]:
                  published.remove(p)
                  failed.add(f'{p["id"]}.{p["arch"]}.toml')
                  rejected.append((f'{p["id"]}.{p["arch"]}.toml', ["its depends lead back to itself (a cycle)"]))
                  changed_any = True
                  break
              if x not in seen:
                  seen.add(x)
                  todo += graph.get((x, p["arch"], p["abi"]), [])
      if not changed_any:
          break

    packages = sorted(published, key=lambda p: (p["id"], p["arch"]))
    outdir = Path(args.out_dir)
    # First publication dates. Without a history nothing is dated: a date
    # made up here would announce every package as new.
    history = None
    if args.history:
        import catalogue_history as ch
        from datetime import datetime, timezone
        try:
            history = ch.load(args.history)
        except (OSError, ValueError) as exc:
            sys.exit(f"{args.history}: {exc}. Create it with catalogue_history.py bootstrap; "
                     "it is not made up from today's date")
        day = args.today or datetime.now(timezone.utc).strftime("%Y-%m-%d")
        new_keys = ch.update(history, packages, day, args.commit[:12])
        for k in new_keys:
            print(f"  first published: {k} ({day})")
        for p in packages:
            v, a = ch.dates_for(history, p)
            if v: p["first_added"] = v
            if a: p["app_first_added"] = a
    v3_path, v2_path, old_path = outdir / "index-v3.json", outdir / "index-v2.json", outdir / "index.json"
    v2_packages = [p for p in packages if not p.get("client_features")]
    # Clients up to 0.3.2 fetch index.json and read at most OLD_CLIENT_MAXTOK
    # values. It keeps the packages it already lists, with their metadata
    # brought up to date from the same manifests (Archives replaces files
    # under the same URL, so an old hash would fail the download); packages
    # new since then go to index-v2.json only. Nothing is shortened to fit.
    if old_path.exists():
        prev = json.loads(old_path.read_text(encoding="utf-8"))
        # By (id, arch, abi): a build for another ABI of a package it lists is
        # new, so it stays out, and index.json keeps one entry per (id, arch),
        # which is all clients up to 0.3.2 were tested with.
        keep = {(p["id"], p["arch"], p.get("abi")) for p in prev.get("packages", [])}
        old_packages = [p for p in v2_packages if (p["id"], p["arch"], p["abi"]) in keep
                        or p["id"] in OLD_CLIENT_ALWAYS]
    else:
        old_packages = v2_packages
    # Only what those clients read; everything else costs them values.
    old_packages = [{k: p[k] for k in OLD_CLIENT_FIELDS if k in p} for p in old_packages]
    # index-v3.json: everything, for clients from 0.6. index-v2.json, read by
    # 0.4 and 0.5: what they can install as described, so nothing that needs
    # a client feature they lack (they would skip the dependencies and call
    # the install a success).
    texts = {v3_path: json.dumps({"schema": 1, "source": cat.CATALOGUE, "packages": packages}, indent=2) + "\n"}
    for path, pkgs in ((v2_path, v2_packages), (old_path, old_packages)):
        texts[path] = json.dumps({"schema": 1, "source": cat.CATALOGUE, "packages": pkgs},
                                 indent=2) + "\n"

    print(f"manifests read         {len(files)}")
    print(f"published              {len(published)}")
    print(f"rejected               {len(rejected)}")
    if rejected:
        by_reason = {}
        for _, probs in rejected:
            by_reason[probs[0]] = by_reason.get(probs[0], 0) + 1
        for reason, n in sorted(by_reason.items(), key=lambda kv: -kv[1]):
            print(f"  {n:>4}  {reason}")
        if args.verbose:
            print()
            for name, probs in rejected:
                print(f"  {name}: {'; '.join(probs)}")
    stop = []
    if failed:
        stop.append(f"{len(failed)} approved manifest(s) not published: {', '.join(sorted(failed))}")
    for path, maxtok, maxbytes, who in ((v3_path, CLIENT_MAXTOK, CLIENT_MAXBYTES, "clients from 0.6"),
                                        (v2_path, CLIENT_MAXTOK, CLIENT_MAXBYTES, "clients 0.4 and 0.5"),
                                        (old_path, OLD_CLIENT_MAXTOK, OLD_CLIENT_MAXBYTES, "clients up to 0.3.2")):
        ntok, nbytes = json_values(json.loads(texts[path])), len(texts[path].encode())
        npk = len(json.loads(texts[path])["packages"])
        print(f"{path.name:<16} {npk} packages, {ntok} JSON values ({100 * ntok // maxtok}% of {maxtok}), "
              f"{nbytes} bytes ({100 * nbytes // maxbytes}% of {maxbytes}); for {who}")
        if ntok > maxtok or nbytes > maxbytes:
            stop.append(f"{path.name} is larger than {who} read. Its packages are kept and "
                        f"nothing is shortened: decide which to drop from it by hand")
    if len(v2_packages) < len(packages):
        print(f"  {len(packages) - len(v2_packages)} package(s) need a client from 0.6: only in index-v3.json")
    if len(old_packages) < len(v2_packages):
        print(f"  {len(v2_packages) - len(old_packages)} package(s) only in index-v2.json and index-v3.json")
    old_ntok = json_values(json.loads(texts[old_path]))
    if old_ntok > OLD_CLIENT_BUDGET * OLD_CLIENT_MAXTOK:
        stop.append(f"index.json would hold {old_ntok} values, past the {int(OLD_CLIENT_BUDGET * 100)}% "
                    f"margin kept below the {OLD_CLIENT_MAXTOK} clients up to 0.3.2 read. Decide by hand "
                    "which packages it stops listing; the package manager's own entries must stay")
    # The way out for clients without self-update: every build of the
    # package manager that index-v2.json has must be in index.json too.
    have_old = {(p["id"], p["arch"], p.get("abi")) for p in old_packages}
    for p in packages:
        if p["id"] in OLD_CLIENT_ALWAYS and (p["id"], p["arch"], p["abi"]) not in have_old:
            stop.append(f"index.json lacks {p['id']} {p['arch']}/{p['abi']}: clients up to 0.3.2 "
                        "would have no way to a current version")
    if stop:
        for why in stop:
            print(f"\nNOT WRITTEN: {why}")
        print(f"{v3_path.name}, {v2_path.name} and {old_path.name} are unchanged.")
        sys.exit(1)
    # Each written beside and renamed over, so a failure half-way cannot leave
    # a truncated index where a good one was.
    outdir.mkdir(parents=True, exist_ok=True)
    if history is not None:
        texts[outdir / "history.json"] = ch.dump(history)
    for path, text in texts.items():
        tmp = path.with_name(path.name + ".part")
        tmp.write_text(text, encoding="utf-8")
        tmp.replace(path)
        print(f"written {path}")


def json_values(v):
    """What the client's reader counts: every object, array, key and value."""
    if isinstance(v, dict):
        return 1 + sum(1 + json_values(x) for x in v.values())
    if isinstance(v, list):
        return 1 + sum(json_values(x) for x in v)
    return 1


if __name__ == "__main__":
    main()
