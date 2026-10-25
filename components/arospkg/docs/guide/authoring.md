# Publishing your program with apkg-pack

`apkg-pack` turns an AROS program you have already built into a checked
ZIP that apkg can install. It then prepares the catalogue entry from the
URL where you uploaded the ZIP.

- Your source code and repository can stay private. Only the finished ZIP
  is published, wherever you normally publish it.
- Making the package needs no GitHub account, no AI assistant and no
  network. GitHub is used only by `submit --pr`.
- `build` packs your program; it does not compile it.
- `check` confirms the description and the archive layout. It does not run
  your program and does not show that it works on any AROS.

## Install

You need Python 3.11 or later on macOS, Linux or Windows. `init`, `check`
and `build` need nothing else.

Download [`apkg-pack.pyz`](https://github.com/tomaszstaniak/arospkg/releases/download/apkg-pack-v0.1.0/apkg-pack.pyz)
(0.1.0, [SHA256SUMS](https://github.com/tomaszstaniak/arospkg/releases/download/apkg-pack-v0.1.0/SHA256SUMS),
[release notes](https://github.com/tomaszstaniak/arospkg/releases/tag/apkg-pack-v0.1.0))
and run it with Python:

```text
python3 apkg-pack.pyz --version
```

On Windows, run `py apkg-pack.pyz`. The examples below write `apkg-pack`
for whichever form you use. To build the file from an arospkg checkout
instead: `python3 tools/make-apkg-pack.py --output apkg-pack.pyz`.

AROS support is experimental. With the aros-cpython 0.1.0 interpreter on
AROS One (ABIv11), `check` and `build` work. That Python opens
bsdsocket.library when it starts, so a TCP/IP stack must be running even
though packing uses no network. Write paths POSIX style:

```text
python apkg-pack.pyz build Demo --output /RAM/demo.zip
python apkg-pack.pyz check /RAM/demo.zip
```

`RAM:demo.zip` is not understood. Other AROS systems are not tested.

A ZIP built from the same files on another computer has the same contents
but may have a different checksum. That is expected: publish the one ZIP
you built, and the catalogue records the checksum of that file.

Optional: `git` and the GitHub CLI `gh`, signed in with `gh auth login`,
for `submit --pr`; `lha` (lhasa) to check `.lha` archives.

## A complete example

### 1. The built drawer

Your build leaves the drawer users get, with its icon beside it:

```text
build/xRick/          the drawer users get
build/xRick.info      its icon
```

The computer you build on does not matter. A program built on an Apple
Silicon Mac for AROS i386 is an i386 package.

### 2. Describe it once

```text
apkg-pack init build/xRick
```

`init` reads the drawer name, the program's `$VER:` string, the CPU of its
executables and the icon, then asks for the rest. It never guesses the ABI.
Enter the one you built for: `v11` for AROS One and current distributions,
`v1` for mainline, `v0` for i386. A binary for one ABI does not run on
another, and the CPU does not tell them apart. Also enter the system
libraries and arospkg packages it needs, and its licence.

The result is `build/xRick.arospkg.toml`, beside the drawer. Keep it with
your project and edit it like any text file; `init` never overwrites it.
In a release script, give every field as an option and add
`--non-interactive` (see `apkg-pack init --help`).

Optional lines shown to the user after installation:

<!-- validate: fragment -->
```toml
post_install_notes = [
  "This port needs the data files of the original game.",
  "Copy them into the Data drawer, or choose their folder at first start.",
]
```

At most 8 lines of 159 characters, printable ASCII without `"` or `\`.
apkg already says where the program was installed and how to start it.

### Other packages: depends, recommends, conflicts, replaces

Only when the program finds the other package in the same apkg root (its
drawer beside yours). A library or a command from the system is a
`requires_system`, not a dependency. Write the plain keys before any
`[[table]]`:

<!-- validate: fragment -->
```toml
replaces = ["hardcode"]

[[recommends]]
id          = "limpet"
min_version = "0.4.1"
reason      = "full-screen front end with colours, scrollback and a clean exit"
```

`depends`, `conflicts` and `replaces` make the entry need apkg 0.6: it is
published only for 0.6 and later. A rename is `replaces` plus your
program's own copy of its old settings; apkg moves none. The meanings are in
the [metadata reference](metadata.md#relations).

### Say how it starts

Add a `[launch]` table and users can start the program with `apkg run`
and PkgManager's **Run**, even when they did not start it from Wanderer
before. Name the program and say whether it is a Workbench or a Shell
program; nothing else. The stack and tool types stay in the program's icon,
which is what a Workbench start uses.

<!-- validate: fragment -->
```toml
[launch]
executable = "Micropolis"     # relative to the drawer
mode       = "workbench"      # needs Micropolis.info beside it, a tool icon
```

The three kinds this was tested with on AROS One:

| program | `[launch]` | why |
|---|---|---|
| Folio, GrafX2 (need an 8 MiB stack) | `executable = "Folio"`, `mode = "workbench"` | the icon sets the stack, so no `Stack` command is needed |
| grep | `executable = "Grep"`, `mode = "shell"` | a command-line tool: arguments come from the user, its return code goes back to the caller |
| DirTree | `executable = "C/DirTree"`, `mode = "shell"` | the program is in a sub-drawer; it starts there unless `working_directory` says otherwise |

`apkg-pack check` reports what it found: the program, its CPU, the icon and
the stack it sets:

```text
xRick.arospkg.toml: note: launch: xRick.info sets the stack: 262144 bytes
```

and refuses what would not start: a program that is not in the drawer, one
for another CPU, a Workbench start without a tool icon, a path leaving the
drawer. `build` writes `application.json` into the drawer in the ZIP, so a
user who unpacks it by hand can still `apkg run` it. Leave `[launch]` out
for a package with nothing to start (a library, a data set); everything
else works as before. Details: [metadata guide](metadata.md) and
[RFC 0002](../rfc/0002-application-folder.md).

### 3. Check and build

```text
apkg-pack check build/xRick
apkg-pack build build/xRick --output xrick-1.0.2.x86_64-aros-v11.zip
apkg-pack check xrick-1.0.2.x86_64-aros-v11.zip
```

`check` names the field and the reason for every problem:

```text
xRick.arospkg.toml: post_install_notes[1]: 184 characters; maximum is 159
```

The ZIP holds the drawer (empty drawers included), its icon, and your
description as `.arospkg/manifest.toml`. Files such as `.DS_Store` are left
out; every other file is stored byte for byte. The same files, description
and timestamp give the same ZIP on the same computer. `SOURCE_DATE_EPOCH`
or `--epoch` sets the timestamp.

Test the program on its target system before you publish it; see
[Testing on AROS](packaging.md#5-testing-on-aros).

### 4. Upload

Upload the ZIP unchanged to [AROS Archives](https://archives.aros-exec.org)
or as a GitHub release asset, and copy its public HTTPS download URL. Use a
new URL for each release where you can.

Uploading does not change the arospkg catalogue. The next step does.

### 5. Submit

```text
apkg-pack submit <download-url> --pr
```

`submit` downloads the archive, reads the description inside it, measures
its size and SHA-256, and opens a pull request in
[arospkg-index](https://github.com/tomaszstaniak/arospkg-index) with the
entry. It uses your existing `gh` login and goes through your fork if you
cannot push. Submitting the same release again opens no second pull
request. You never type the size or checksum. `submit --pr` is tested on
macOS and Linux; on Windows it is not tested yet.

Without GitHub, preview the entry in a local copy of the catalogue and send
it to the maintainers another way, such as an issue or an e-mail:

```text
git clone https://github.com/tomaszstaniak/arospkg-index.git
apkg-pack submit <download-url> --index arospkg-index --dry-run
```

### 6. After you submit

An automatic check downloads the archive again and runs the catalogue's
review on it: the entry's metadata, size and SHA-256, a readable archive,
safe member names that cannot overwrite one another, the drawer, icon and
program the entry names, and the CPU of the programs. A maintainer reads
the entry and merges it. The catalogue is then regenerated, and users see
the package the next time apkg or PkgManager refreshes its index. Nothing
is published before the merge.

A published entry is **Listed**. It becomes **Reviewed** when the review
passed for its archive, and only then: the status is written by the
catalogue's own workflow, for those exact bytes and that entry, and a pull
request that writes it by hand is refused. Nobody has to install or start
your program before it is published. If you or a maintainer did run it,
that is recorded apart, as a runtime test with the system and the date (see
[Notes for maintainers](#notes-for-maintainers)); a new archive starts
without one.

### 7. The next release

Build as usual, then edit the description you kept:

- a new upstream release: set `version` to the upstream version and remove
  `revision`;
- a new AROS package of the same version, such as a packaging fix or a
  rebuild: raise `revision`.

`version` is the program's own version. `revision` numbers the AROS
packages of that version. apkg-pack changes neither. Then repeat steps 3
to 5. Changed bytes need a new version or revision: `submit` refuses a
different file under one that is already published.

To pack from a release script, run the same three commands after the
build. [Folio's release script](https://github.com/tomaszstaniak/aros-foliopdf/blob/main/scripts/make-release.sh)
is a working example. It keeps one description per target in
`packaging/catalogue/` and stops if its version does not match the release.

## Notes for maintainers

- Archives without `.arospkg/manifest.toml`, including those already on
  AROS Archives, keep their maintainer-written manifests in `manifests/`.
- A correction to an author's entry goes in `overrides/` under the
  manifest's file name: any field except id, arch, abi, url, size and
  sha256. It is applied each time the catalogue is made and survives the
  author's next submission.
- A runtime test goes in `runtime-tests/<id>.<arch>.<abi>.toml`, one
  `[[test]]` per test, naming the SHA-256 of the archive that was run (see
  the [metadata reference](metadata.md#runtime-tests)). The catalogue shows
  only the tests made with the bytes it currently offers.
- `review/` and `history.json` are written by the publish workflow. Do not
  edit them; the check refuses a pull request that does. After a change of
  the review rules or of upstream files, run the publish workflow by hand
  with *review_all*: every entry is reviewed again, and one that no longer
  passes stays Listed and is named in the log.
- When an upstream file changes under a published URL, the download no
  longer matches the entry and installing it fails. Measure the new file,
  then publish it under a new `revision` (same program version) or a new
  `version`; the old runtime tests do not carry over. When the new file
  cannot be reconciled, set `excluded = "<the reason>"` in the manifest.
- A variant is its id, CPU and ABI: `xrick.x86_64.v11.toml` and
  `xrick.x86_64.v1.toml` can both be published. Older files named
  `<id>.<arch>.toml` are still read.
- Do not edit `index.json` or `index-v2.json`; they are generated.

See the [packaging guide](packaging.md) for the archive layout and the
[metadata reference](metadata.md) for every field.
