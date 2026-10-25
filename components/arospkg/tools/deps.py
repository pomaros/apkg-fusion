#!/usr/bin/env python3
r"""Find CANDIDATE library dependencies in a package's binaries.

The long-open question was whether dependency discovery can be mechanised at
all, rather than written by hand for 168 candidates. Measured 2026-09-07: it
can be *assisted*, which is worth a great deal. It cannot be automated, and
this tool must not be read as if it were.

AROS executables usually name the libraries they open as literal strings,
because `OpenLibrary("foo.library", 0)` needs the name at runtime. So a plain
`strings` recovers many of them with no debug information and without running
AROS:

    strings <binary> | grep -Eio '[a-z0-9_.-]+\.library'

What that produces is a **list of candidates in both directions**, and the
error runs both ways:

  * It can MISS a dependency. A name can be constructed at runtime, read from a
    configuration file, or supplied by a plugin. Nothing here sees a dependency
    on an external command, a data file, a datatype, or a device.
  * It can INVENT one. A literal may sit in a code path that never executes, or
    behind an optional feature the program runs happily without. The presence of
    the string does not prove the program fails without the library.
  * A statically linked dependency is invisible by construction: SDLPoP never
    references SDL at all, because SDL2 is linked in.

Subtraction has the same limit. The base set is read from one build's `Libs/`
directory, so "in the base set" means **available in that build** -- not
guaranteed on every mainline installation, and certainly not on a distribution.

So the honest output is "N references found, M of them not in the reference
image", which is what this prints. Turning that into a dependency, a system
requirement, or nothing at all is a judgement, and `depends_checked = true` in
a manifest asserts that a human made it with this scan in front of them.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# In ROM or supplied by the toolchain; never present in Libs/.
ALWAYS_PRESENT = {
    "exec.library", "utility.library", "aros.library",
    "crt.library", "stdlib.library", "m.library",
}

LIB_RE = re.compile(rb"[A-Za-z0-9_.-]+\.library")
DEFAULT_SDK_LIBS = "/Volumes/arosmain/build/bin/pc-x86_64/AROS/Libs"


def base_libraries(libs_dir):
    d = Path(libs_dir)
    if not d.is_dir():
        return None
    return {p.name.lower() for p in d.iterdir() if p.name.lower().endswith(".library")}


def refs_in(path):
    """Library names referenced by one file."""
    try:
        data = Path(path).read_bytes()
    except OSError:
        return set()
    return {m.decode("ascii").lower() for m in LIB_RE.findall(data)}


def is_executable_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="extracted package directories or binaries")
    ap.add_argument("--libs", default=DEFAULT_SDK_LIBS,
                    help="the build's Libs/ directory, used as the base set")
    ap.add_argument("--all", action="store_true", help="also list base-system references")
    args = ap.parse_args()

    print(f"reference image: {args.libs}")
    print("A name found here is available in THAT build. It is not a guarantee")
    print("for every mainline installation, and says nothing about distributions.\n")
    base = base_libraries(args.libs)
    if base is None:
        print(f"warning: {args.libs} not found; base set is the curated list only,\n"
              f"         so almost everything will look like a dependency.", file=sys.stderr)
        base = set()
    known = base | ALWAYS_PRESENT

    for target in args.paths:
        p = Path(target)
        files = [p] if p.is_file() else sorted(q for q in p.rglob("*") if q.is_file())
        binaries = [q for q in files if is_executable_elf(q)]
        found = {}
        for q in binaries:
            for r in refs_in(q):
                found.setdefault(r, []).append(q.name)

        unresolved = {r: v for r, v in found.items() if r not in known}
        print(f"=== {target}")
        print(f"    binaries scanned   {len(binaries)}")
        print(f"    references found   {len(found)}")
        if args.all:
            for r in sorted(found):
                tag = "base" if r in known else "DEPENDENCY"
                print(f"      {r:<26} {tag}")
        if unresolved:
            print(f"    not in the reference image ({len(unresolved)}) -- judge each by hand:")
            for r in sorted(unresolved):
                print(f"      {r:<26} referenced by {', '.join(sorted(set(unresolved[r])))}")
        else:
            print("    all references are in the reference image")
        print("    (candidates only: dynamic names are missed, dead literals are"
              " reported,\n     static linkage is invisible)")
        print()


if __name__ == "__main__":
    main()
