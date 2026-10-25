"""The standalone apkg-pack.pyz, as an author gets it: built from this
checkout, copied to a directory with a space in its name, and run with an
isolated Python from a directory outside the checkout, with no git or gh on
PATH and no network. init, check, build, check of the archive, a second
build to the same bytes, and submit --dry-run into a scratch index.

Runs on macOS, Linux and Windows (.github/workflows/apkg-pack.yml runs it on all
three). Nothing here runs on AROS."""
import hashlib, os, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
fails = []


def elf(machine=62, ver=b"$VER: My App 2.1 (01.01.2026)"):
    h = bytearray(64); h[:4] = b"\x7fELF"; h[4] = 2; h[5] = 1
    h[18:20] = machine.to_bytes(2, "little")
    return bytes(h) + b"\0" * 64 + ver + b"\0"


with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    tooldir = d / "my tools"
    tooldir.mkdir()
    pyz = tooldir / "apkg-pack.pyz"
    subprocess.run([sys.executable, str(HERE / "tools/make-apkg-pack.py"), "--output", str(pyz)],
                   check=True, capture_output=True)

    # PATH holds nothing but this Python's own directory: no git, no gh.
    env = {k: v for k, v in os.environ.items() if k.upper() in ("SYSTEMROOT", "TEMP", "TMP", "HOME", "USERPROFILE")}
    env["PATH"] = str(Path(sys.executable).parent)
    work = d / "work dir"
    work.mkdir()

    def run(*args):
        r = subprocess.run([sys.executable, "-I", str(pyz)] + [str(a) for a in args],
                           capture_output=True, text=True, cwd=work, env=env)
        return r.returncode, r.stdout + r.stderr

    code, out = run("--version")
    if code or not out.startswith("apkg-pack "):
        fails.append(f"--version: {out}")

    proj = work / "build out"
    dr = proj / "My App"
    (dr / "Data" / "Saves").mkdir(parents=True)          # an empty drawer must survive packing
    (dr / "Catalogs").mkdir()
    (dr / "MyApp").write_bytes(elf())
    (dr / "MyApp.info").write_bytes(b"\xe3\x10\x00\x01program icon")
    (dr / "Read Me.txt").write_bytes(b"line one\r\nline two\n")   # bytes kept as they are
    (dr / "Thumbs.db").write_bytes(b"junk")
    (proj / "My App.info").write_bytes(b"\xe3\x10\x00\x01drawer icon")

    code, out = run("init", dr, "--non-interactive", "--id", "myapp", "--summary", "Test application",
                    "--kind", "app", "--abi", "v11", "--no-depends", "--requires", "muimaster.library",
                    "--license", "MIT")
    if code:
        fails.append(f"init: {out}")
    manifest = proj / "My App.arospkg.toml"
    if not manifest.is_file() or 'version         = "2.1"' not in manifest.read_text():
        fails.append(f"init did not take the version from $VER: {out}")
    if 'icon            = "My App.info"' not in manifest.read_text():
        fails.append("init did not find the drawer icon beside the drawer")

    code, out = run("check", dr)
    if code or "ok: myapp 2.1 for x86_64/v11" not in out:
        fails.append(f"check drawer: {out}")

    z1, z2 = work / "out" / "myapp 2.1.zip", work / "out" / "again.zip"
    z1.parent.mkdir()
    code, out = run("build", dr, "--output", z1)
    if code:
        fails.append(f"build: {out}")
    code, out = run("check", z1)
    if code or "ok: myapp 2.1" not in out:
        fails.append(f"check archive: {out}")
    run("build", dr, "--output", z2)
    if z1.read_bytes() != z2.read_bytes():
        fails.append("two builds of the same drawer differ")

    with zipfile.ZipFile(z1) as z:
        names = set(z.namelist())
        want = {"My App/", "My App/Data/", "My App/Data/Saves/", "My App/Catalogs/", "My App/MyApp",
                "My App/MyApp.info", "My App/Read Me.txt", "My App.info",
                ".arospkg/", ".arospkg/manifest.toml"}
        if names != want:
            fails.append(f"archive members: extra {sorted(names - want)}, missing {sorted(want - names)}")
        for n in ("My App/MyApp", "My App/MyApp.info", "My App/Read Me.txt"):
            if n in names and z.read(n) != (proj / n).read_bytes():
                fails.append(f"{n}: bytes changed in the archive")
        if ".arospkg/manifest.toml" in names and z.read(".arospkg/manifest.toml") != manifest.read_bytes():
            fails.append("the embedded manifest is not the author's file")

    code, out = run("build", dr, "--output", dr / "self.zip")
    if code == 0:
        fails.append("build into the drawer itself must refuse")

    # submit with neither --pr nor --index: a clear refusal, not a crash.
    code, out = run("submit", "https://example.org/myapp.zip", "--use-local-copy", z1, "--dry-run")
    if code == 0 or "--index" not in out:
        fails.append(f"submit without an index must say what it needs: {out}")
    index = work / "index"
    (index / "manifests").mkdir(parents=True)
    code, out = run("submit", "https://example.org/myapp.zip", "--use-local-copy", z1,
                    "--index", index, "--dry-run")
    sha = hashlib.sha256(z1.read_bytes()).hexdigest()
    if code or f'sha256          = "{sha}"' not in out or "manifests/myapp.x86_64.v11.toml" not in out:
        fails.append(f"submit --dry-run: {out}")
    if any((index / "manifests").iterdir()):
        fails.append("submit --dry-run wrote a file")

    # The host's CPU is not the target's: an i386 program packed here stays i386.
    dr2 = proj / "Old"
    dr2.mkdir()
    (dr2 / "Old").write_bytes(elf(machine=3, ver=b"$VER: Old 1.0 (01.01.2026)"))
    code, out = run("init", dr2, "--non-interactive", "--summary", "i386 program", "--kind", "tool",
                    "--abi", "v0", "--no-depends", "--no-requires")
    if code or 'arch            = "i386"' not in (proj / "Old.arospkg.toml").read_text():
        fails.append(f"init of an i386 drawer: {out}")
    code, out = run("init", proj / "My App", "--non-interactive")
    if code == 0 or "already exists" not in out:
        fails.append("init must not overwrite a manifest")

if fails:
    print("\n".join(fails))
    sys.exit(1)
print("APKG-PACK STANDALONE: built, run isolated outside the checkout without git or gh; "
      "spaces, icons, empty drawers, same bytes twice")
