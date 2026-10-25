#!/usr/bin/env python3
"""Large generated indexes, for the capacity of the client's JSON reader.

    make-big-index.py N OUTFILE [--target-revision R]

N packages shaped like the published catalogue (2026-10-04: about 50 JSON
values and 874 bytes per package): summaries, categories, requirements, a
tenth with post_install_notes, and one in twenty-five offered for several
targets with the variants in shuffled order. The fixed seed makes the same
file every time. Every generated entry is checked with mkindex's own notes
rule. The last entry, "zz-target", is the real gmore x86_64 ABIv11 archive
from AROS Archives (the same one as tests/ux), so a guest can install it
from the end of a large index; --target-revision raises its revision for an
upgrade test. Nothing here is published.
"""
import argparse, json, random, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mkindex  # noqa: E402

TARGET = {
    "url": "https://archives.arosworld.org/share/utility/text/gmore.x86_64-aros-v11.zip",
    "size": 35787,
    "sha256": "65ecebd838705d29ffdb05dbd4727aa8e9c8f49342923a73e30d60acb6427131",
    "subdir": "GMore1.2", "icon": "GMore1.2.info",
}
CATS = ["game/puzzle", "game/action", "utility/text", "utility/file", "office/misc",
        "graphics/edit", "audio/play", "network/web", "development/language", "demo/intro"]
WORDS = ("viewer editor player converter game clone port tool manager browser "
         "calculator tracker monitor archive font image sound shell client").split()
TARGETS = [("x86_64", "v11"), ("x86_64", "v1"), ("i386", "v0"), ("aarch64", "v1")]
LIBS = ["crt.library", "stdlib.library", "z1.library", "png.library", "SDL2.library"]


def entry(rng, i, arch="x86_64", abi="v11"):
    pid = f"pkg{i:05d}"
    e = {
        "id": pid, "version": f"{rng.randint(0, 9)}.{rng.randint(0, 20)}",
        "revision": rng.choice([0, 0, 0, 1, 2]),
        "summary": " ".join(rng.choice(WORDS) for _ in range(rng.randint(3, 9))).capitalize(),
        "category": rng.choice(CATS), "kind": "app", "arch": arch, "abi": abi,
        "url": f"https://archives.example.invalid/share/{pid}.{arch}-aros-{abi}.lha",
        "size": rng.randint(20_000, 20_000_000),
        "sha256": "%064x" % rng.getrandbits(256),
        "subdir": pid.capitalize(), "icon": pid.capitalize() + ".info",
        "license": rng.choice(["GPL-2.0", "MIT", "freeware", "BSD-3-Clause"]),
        "source": f"https://example.invalid/{pid}",
        "depends": [],
        "requires_system": [{"type": "library", "id": lib}
                            for lib in rng.sample(LIBS, rng.randint(0, 3))],
    }
    if rng.random() < 0.1:
        e["post_install_notes"] = [
            "This port needs the data files of the original release.",
            f"Copy them into the Data drawer of {pid}, or choose their folder at first start.",
        ]
        assert not mkindex.check_notes(e["post_install_notes"])
    return e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("n", type=int)
    ap.add_argument("out")
    ap.add_argument("--target-revision", type=int, default=0)
    a = ap.parse_args()
    rng = random.Random(20261004)
    pkgs = []
    for i in range(a.n - 1):
        if i % 25 == 0:
            vs = [entry(random.Random(i), i, ar, ab) for ar, ab in TARGETS]
            rng.shuffle(vs)
            pkgs.extend(vs)
        else:
            pkgs.append(entry(rng, i))
    t = {"id": "zz-target", "version": "1.2", "revision": a.target_revision,
         "summary": "Text viewer similar to MORE (gmore, test entry at the end)",
         "category": "utility/text", "kind": "app", "arch": "x86_64", "abi": "v11",
         **TARGET, "depends": [],
         "post_install_notes": ["Test note: installed from the end of a generated index."],
         "requires_system": [{"type": "library", "id": "crt.library"}]}
    pkgs.append(t)
    with open(a.out, "w") as f:
        json.dump({"schema": 1, "generated": "fixture", "packages": pkgs}, f, indent=1)
        f.write("\n")


main()
