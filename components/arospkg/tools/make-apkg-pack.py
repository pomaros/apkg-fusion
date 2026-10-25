#!/usr/bin/env python3
"""Build apkg-pack.pyz: the author tool as one file that runs with any
Python 3.11 or later, without an arospkg checkout.

    tools/make-apkg-pack.py [--output dist/apkg-pack.pyz]

Its version is the nearest apkg-pack-vX.Y.Z tag (git describe). Release it
on such a tag, never as the repository's latest release: apkg self-update
downloads from releases/latest.

The archive holds the files this checkout runs, unchanged: apkg-pack (as
apkg_pack.py), mkindex.py and catalogue.py, whose rules the catalogue CI
uses too, publish_github.py for submit --pr, and the licence. Nothing is
rewritten, so the standalone tool cannot drift from the checked-in one.
Members are written in a fixed order with a fixed time: the same commit
gives the same bytes with the same Python zlib.
"""
import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEMBERS = {                       # name in the archive: file in this checkout
    "apkg_pack.py": "tools/apkg-pack",
    "mkindex.py": "tools/mkindex.py",
    "catalogue.py": "tools/catalogue.py",
    "publish_github.py": "tools/publish_github.py",
    "LICENSE": "LICENSE",
}
MAIN = b"""import sys
if sys.version_info < (3, 11):
    sys.exit("apkg-pack needs Python 3.11 or later; this is " + sys.version.split()[0])
import apkg_pack
apkg_pack.main()
"""
STAMP = (1980, 1, 1, 0, 0, 0)


def version():
    try:
        # apkg-pack is released on its own tags, apart from the client's vX.Y.Z.
        v = subprocess.run(["git", "-C", str(ROOT), "describe", "--tags", "--match", "apkg-pack-v*",
                            "--always", "--dirty"],
                           capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        v = "unknown"
    return v.removeprefix("apkg-pack-v")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--output", default=str(ROOT / "dist" / "apkg-pack.pyz"))
    a = ap.parse_args()
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    v = version()
    tmp = out.with_name(out.name + ".part")
    with open(tmp, "wb") as f:
        f.write(b"#!/usr/bin/env python3\n")
        with zipfile.ZipFile(f, "w") as z:
            def add(name, data):
                zi = zipfile.ZipInfo(name, STAMP)
                zi.create_system = 3
                zi.external_attr = 0o644 << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(zi, data, compresslevel=9)
            add("__main__.py", MAIN)
            add("_apkg_pack_build.py", f'VERSION = "{v}"\n'.encode())
            for name, src in MEMBERS.items():
                add(name, (ROOT / src).read_bytes())
    tmp.chmod(0o755)
    tmp.replace(out)
    print(f"written {out}: apkg-pack {v}")


if __name__ == "__main__":
    sys.exit(main())
