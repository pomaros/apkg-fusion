---
title: "Packaging a program for arospkg"
status: guide
describes: arospkg 0.3 (apkg 40c2ba93…) and tools/mkindex.py, 2026-09-29
audience: application authors and index maintainers
---

# Packaging a program for arospkg

This guide covers what works today: going from a drawer holding your program
to an archive, and from there to an approved entry in the index. Field
meanings are in the [metadata reference](metadata.md). What is
proposed but not implemented is in the [RFCs](../rfc/). This guide describes
neither.

Every command and output below comes from one real run, on 2026-09-29, of
Micropolis 0.1.0-rc3. The package is our own port, published as a GitHub
release. This is a worked example from an older release; use the authoring guide for the current workflow.

## How it works today, in one paragraph

The client (`apkg`, `PkgManager`) reads one file, the **index**
(`index.json`). That file is generated from **manifests**: one TOML file per
package, in the `manifests/` directory of the
[arospkg-index](https://github.com/tomaszstaniak/arospkg-index) repository,
written and reviewed by whoever maintains the index. The client never reads anything the author puts inside the archive.
If an archive contains `.arospkg/manifest.toml`, the client neither reads it
nor installs it. So a package reaches users through an index manifest, whoever
wrote the program.

There are two roles:

- **An author publishing their own archive.** Sections 1 to 5, then
  [6A](#6a-an-author-publishing-their-own-archive).
- **A maintainer describing someone else's upload**, usually on AROS
  Archives. The author may not know arospkg exists. Section 1 is for reading
  an archive rather than making one. Sections 2 to 5 apply unchanged, then
  [6B](#6b-a-maintainer-describing-someone-elses-upload).

## 1. The archive

### Format

A ZIP or an LHA archive. The client decides which by reading the first bytes,
not the file name.

| | supported |
|---|---|
| ZIP | methods 0 (stored) and 8 (deflate); directory entries |
| LHA | header levels 0, 1 and 2; methods `-lh0-`, `-lh5-`, `-lh6-`, `-lh7-`, and `-lhd-` (a directory entry) |

To create an LHA archive you need an `lha` that can write. Homebrew's
`lhasa` only reads. Micropolis is packed with
`lha aq2o51 <archive> Micropolis Micropolis.info .arospkg`.

### Layout

```
micropolis.x86_64-aros-v11.lha
├── Micropolis.info          the drawer icon                      → icon
├── Micropolis/              the package: everything installed    → subdir
│   ├── Micropolis           the program
│   ├── Micropolis.info      the program icon (tool types, stack)
│   ├── CITY.CTY, CITY.CTY.info
│   ├── ReadMe.txt, BUILD.txt, SHA256SUMS
│   └── cities/ icons/ sounds/ sprites/ startup/ Licenses/ tiles.bmp
└── .arospkg/manifest.toml   optional; the client does not read it
```

When the package is installed under a root, for example the default
`SYS:Packages`:

- the contents of `subdir` go into `<root>/<id>/`. **The installed drawer is
  named after the package `id`, not after `subdir`.** `Micropolis/` becomes
  `SYS:Packages/micropolis/`;
- the file named by `icon` becomes `<root>/<id>.info`, the icon for that
  drawer;
- nothing else in the archive is installed. That covers `.arospkg/`, any
  loose README at the top level, and any other drawer.

There are **two icons, and they are different things**:

- the **drawer icon** sits beside the drawer at the top of the archive, and
  is the one `icon` names;
- the **program icon** sits inside the drawer, next to the program. It is a
  normal file of the package and carries the tool types and the stack size.

An archive may have one, both or neither. The index names only the drawer
icon. If `icon` is left out, the drawer is installed without one. Normally
`icon` names a file at the top level, such as `Micropolis.info`.
`mkindex.py` checks only that the file exists. It cannot tell whether it is
the drawer icon, so the reviewer checks that: open it, or look at it in the
installed drawer.

`subdir = ""` installs the whole top level of the archive as the package, and
that includes `.arospkg/` if it is there. Use it only for archives with no
drawer. See [the reference](metadata.md#subdir).

### What the program must cope with

- **Its drawer is renamed and can be moved.** Find resources through
  `PROGDIR:`, never through a fixed path. Also check defaults that users see:
  the Micropolis save requester opened on `SYS:Micropolis` in the walk. That
  drawer does not exist when the game is installed as
  `SYS:Packages/micropolis`, so the user has to pick a drawer themselves.
- **Protection bits and file comments are not restored.** Every file gets the
  default `rwed`. A script meant to be run by its name needs the `s` bit and
  will not have it. Tell users to start it with `Execute`, as OpenLoco does
  (`Execute Run-OpenLoco`).
- **Empty directories** in the archive are created: `-lhd-` members in LHA
  archives, and directory entries in ZIP. The registry records files only.
  Removal deletes directories that end up empty.
- **The stack.** A plain Shell gives 40960 bytes. If the program needs more,
  put the size in the program icon, and say in the ReadMe what to type in a
  Shell. GrafX2 and Folio need 8 MiB.

### What the client refuses in an archive

Every member is checked before anything is written. A single bad member
refuses the whole archive:

- a path that starts with `/`, or with `\` in a ZIP;
- a path containing `:` anywhere (an AROS volume);
- a `..` path component;
- control characters;
- a member name of 512 bytes or more.

Size limits (`src/libpkg/limits.h`):

| limit | value |
|---|---|
| download | 64 MiB |
| one member, unpacked, in a ZIP | 128 MiB (64 MiB before 0.6) |
| one member, unpacked, in an LHA | 64 MiB |
| all members, unpacked | 256 MiB |
| the drawer icon | 1 MiB |
| files in the package | 2048 |
| a path inside the package | 255 bytes |

## 2. Identity: `id`, `version`, `revision`

<!-- validate: fragment -->
```toml
id       = "micropolis"     # the registry key and the installed drawer's name
version  = "0.1.0-rc3"      # upstream's own string, compared only for equality
revision = 1                # optional: this port's 1st release of that version
```

- **`id`**: lowercase, matching `^[a-z][a-z0-9._-]{0,63}$`. Keep it shorter
  than 64 characters (see the note in the reference). It never changes: a
  different `id` is a different package to the client.
- **`version`** is what upstream calls the release. Use your own number if
  the program is its own upstream. Add nothing to it: no `-aros`, no date.
- **`revision`** counts this port's releases of that `version`. It starts at
  1, and every published correction raises it, whether the program, an icon
  or a line of the manifest changed. Leave it out if there has only been one
  release. The client shows `0.1.0-rc3-aros1`, a suffix it builds itself.

What the client does with them, in 0.3:

- `apkg upgrade` moves to a newer revision of the same `version`, or to a
  newer `version` when both versions can be ordered (0.4.1 and later).
- A version can be ordered when it is dotted numbers, optionally followed by
  one pre-release tag `-testN`, `-previewN` or `-rcN` and then a rebuild tag
  `-rN` (0.6 and later): 0.9.0-test2-r2 < 0.9.0-test4 < 0.9.0, test9 <
  test10. Pre-releases of different kinds for the same numbers, and every
  other spelling, are not ordered, and moving between them is not an
  upgrade: the user removes the package and installs it again. Keep one
  scheme for one program.
- A member over 64 MiB unpacked is refused by clients before 0.6. The
  catalogue lists such an entry only in `index-v3.json`, with the client
  feature `large-members`.

## 3. Target: `arch`, `abi`, `requires_system`

<!-- validate: fragment -->
```toml
arch = "x86_64"
abi  = "v11"
```

- **`arch`**: `x86_64`, `i386` or `aarch64` in 0.3.1. The client refuses any other CPU than
  its own.
- **`abi`**: `v11` for AROS One and other current distributions, and `v1`
  for mainline; 0.3.1 also recognizes `v0` for the i386 ABIv0 line.
  It is required: the client refuses an entry whose `abi` is
  missing or unknown, before downloading anything, and `mkindex.py` refuses
  the manifest. A binary for one ABI
  installs on the other and then crashes. An AROS Archives file name ending
  in `-v11` is ABIv11. Without the marker, it is ABIv1.
- **One build per `id`, `arch` and `abi`.** A v1 and a v11 build of one
  program are two manifests, `<id>.<arch>.v1.toml` and `<id>.<arch>.v11.toml`,
  and the client picks the one for its own target; it refuses to choose if it
  ever sees two for the same target. `index.json`, for clients up to 0.3.2,
  keeps one build per `id` and `arch`: a second ABI goes to `index-v2.json`
  only.

The primary tested release is **ABIv11, x86_64**. Version 0.3.1 also ships
experimental mainline, i386 and aarch64 builds; see its release notes for
their coverage. Recognizing a target is not a claim that every package for
it has been tested.

<!-- validate: fragment -->
```toml
[[requires_system]]
type = "library"
id   = "crt.library"
```

`requires_system` lists what the **machine** must already have. For other
packages in the index, the field is `depends` (see the reference). To find
the requirements:

1. `tools/deps.py <binary or unpacked drawer>` lists the library names it
   finds. It
   suggests candidates and decides nothing: it misses names built at run time
   and reports strings that are never opened.
2. Drop what every AROS has (`dos`, `exec`, `intuition`, `graphics`, …).
3. Check the rest against the systems you target. For a ZIP,
   Compare them with the libraries available on the target system. Check devices
   too.
4. Do not write `min_version` unless you know the version the program needs.
   The archive does not record the versions it was built against, and a guessed
   minimum refuses installs that would work.

For Micropolis this leaves `crt.library`, `m.library` and `stdlib.library`.
AROS One supplies these and mainline does not. AHI is optional: without it
the game is silent.

Only `type = "library"` is checked by the client. Any other type is accepted,
and reported as *undetermined* on every machine. At most 16 entries.

## 4. The manifest

`manifests/micropolis.x86_64.toml` in arospkg-index, in the form used in
the walk. This is an example: the published copy may since have changed. **Put every
plain key above the first `[[requires_system]]` or `[source]` header.** In
TOML, a key written after a table header belongs to that table. Keep every
string to printable ASCII, without `"` or `\`: the 0.3 client reads strings
without decoding escapes, and `mkindex.py` refuses what it would misread.

<!-- validate: manifest -->
```toml
status          = "approved"
id              = "micropolis"
version         = "0.1.0-rc3"
revision        = 1
summary         = "Classic city-building simulation for AROS"
arch            = "x86_64"
abi             = "v11"
category        = "game/strategy"
license         = "GPL-3.0-or-later with EA Section 7 terms; Micropolis Public Name License"
url             = "https://github.com/tomaszstaniak/aros-micropolis/releases/download/v0.1.0-rc3/micropolis.x86_64-aros-v11.lha"
size            = 2593505
sha256          = "c46bbc14274af53fb4452f59081fbcf28f1ce5fffb534c06d169b0b1911cd124"
kind            = "app"

depends         = []
depends_checked = true
subdir          = "Micropolis"
icon            = "Micropolis.info"
installs_on     = ["aros-one"]
runs_on         = ["aros-one"]
does_not_run_on = ["mainline-x86_64"]

[[requires_system]]
type = "library"
id   = "crt.library"

[[requires_system]]
type = "library"
id   = "m.library"

[[requires_system]]
type = "library"
id   = "stdlib.library"

[source]
repository = "https://github.com/tomaszstaniak/aros-micropolis"
revision   = "v0.1.0-rc3"
archive    = "Micropolis-AROS-0.1.0-rc3-source.zip"
sha256     = "969be5094991e8c57b9689da06e68c4082daf4491a907411db0972aa3635e5e5"
built_with = "abiv11-x86_64, GCC 10.5.0"
```

### Size and SHA-256

Measure them on the exact file at `url`, byte for byte:

```
$ wc -c < micropolis.x86_64-aros-v11.lha
 2593505
$ shasum -a 256 micropolis.x86_64-aros-v11.lha
c46bbc14274af53fb4452f59081fbcf28f1ce5fffb534c06d169b0b1911cd124  micropolis.x86_64-aros-v11.lha
```

The client checks both before it keeps a download. The hash is what fixes the
package. If the file at `url` ever changes, every install of this entry fails,
and that is intended.

### Validation

`tools/mkindex.py` is the validator: it publishes only manifests that pass. To
check a manifest on its own, give it a directory of its own and a scratch
output. Put the archive in the `--cache` directory, so that `subdir` and
`icon` are checked against its real listing:

```
$ mkdir -p draft/manifests draft/cache
$ cp micropolis.x86_64.toml draft/manifests/
$ cp micropolis.x86_64-aros-v11.lha draft/cache/
$ python3 tools/mkindex.py --manifests draft/manifests --cache draft/cache \
      --out draft/index.json --verbose
```

That run used the generator of 0.3.1. The current one takes `--out-dir
draft` instead of `--out`, writes `draft/index-v2.json` and
`draft/index.json`, and prints one size line for each in place of `client
capacity`; the rest of the output below is unchanged.

With `status = "skeleton"` it leaves the manifest out, which is normal,
and exits 0:

```
  note: micropolis: license is 72 characters; the client shows 63
manifests read         1
published              0
rejected               1
     1  status is 'skeleton', not "approved"

  micropolis.x86_64.toml: status is 'skeleton', not "approved"
client capacity        7 of 8192 JSON values, 105 of 4194304 bytes

written draft/index.json
```

With `status = "approved"` it publishes it and exits 0:

```
  note: micropolis: license is 72 characters; the client shows 63
manifests read         1
published              1
rejected               0
client capacity        68 of 8192 JSON values, 1292 of 4194304 bytes

written draft/index.json
```

The note is about display only: `show` cuts the licence, and the index
keeps it whole. With an approved manifest that fails, here with `url`
changed to `http://`, it writes nothing and exits 1:

```
manifests read         1
published              0
rejected               1
     1  url is not https://; the client fetches nothing else

  micropolis.x86_64.toml: url is not https://; the client fetches nothing else
client capacity        7 of 8192 JSON values, 105 of 4194304 bytes

NOT WRITTEN: 1 approved manifest(s) not published: micropolis.x86_64.toml
draft/index.json is unchanged.
```

The `client capacity` line shows how much of what the 0.3 client can read
the index uses. The generator after 0.3.2 prints `index size` instead, with
two shares: of what clients up to 0.3.2 read (8192 JSON values, 4 MiB, a
fixed array in those clients) and of what later clients read (they allocate
what the file needs, up to 1048576 values and 16 MiB as a guard against a
broken file). `index.json` keeps only the packages it already listed, with
their metadata brought up to date; new packages go to `index-v2.json` only.
If the packages `index.json` lists outgrow the old limit, the generator
writes neither file and says so; it never drops entries or shortens text to
fit. If the archive is not in the cache, `mkindex.py` prints
`warning: … subdir and icon unchecked` and goes on without that check.

What it checks, and what it leaves to a person, is in
[the reference](metadata.md#what-is-checked-where).

## 5. Testing on AROS

Publication does not require that we start the program: an approved entry
is **Listed**, and the automatic review decides whether it is **Reviewed**
(see [Listed, Reviewed and runtime tests](metadata.md#listed-reviewed-and-runtime-tests)).
A hash and a clean unpack only show that the archive is intact, so a test on
the target system is still the only way to know that it runs. When you make
one, record it as a runtime test, for the archive you ran. Test with the
generated draft index and a root of your own, so the machine's real packages
are not touched:

```
apkg --root SYS:Walk --index RAM:index.json <command>
```

`--index` takes a local file, so nothing has to be published first. The
walk ran on AROS One 1.3 x86_64 (ABIv11) under QEMU, from a Shell at the
default 40960-byte stack, with `apkg 0.3` from the release archive:

```
apkg 0.3
  abi     v11
  binary  40c2ba9350bc10e6da294d0cf0675aefb8f3dc86c519fd8c992bb4bf675609e0
```

### 5.1 What the client will do: `show`

`show` changes nothing:

```
1.AROS:> apkg --root SYS:Walk --index RAM:index.json show micropolis
micropolis 0.1.0-rc3-aros1
  Classic city-building simulation for AROS
  category:     game/strategy
  target:       x86_64, ABI v11
  compatibility: native -- built for this machine: x86_64, ABIv11, current distributions such as AROS One
  download:     2533 KB from https://github.com/tomaszstaniak/aros-micropolis/releases/download/v0.1.0-rc3/micropolis.x86_64-aros-v11.lha
  sha256:       c46bbc14274af53fb4452f59081fbcf28f1ce5fffb534c06d169b0b1911cd124
  source:       https://github.com/tomaszstaniak/aros-micropolis
  license:      GPL-3.0-or-later with EA Section 7 terms; Micropolis Public Nam
  requirements: satisfied, probed now:
    crt.library: satisfied (already open, version 5)
    m.library: satisfied (already open, version 1)
    stdlib.library: satisfied (already open, version 3)
  installed:    no
```

Check each line against what you meant. `compatibility` must be `native`.
Every requirement must be `satisfied` on a machine where the program runs.
The licence line stops at 63 characters: that is a display limit in 0.3, and
the index holds the whole string.

### 5.2 Install

```
1.AROS:> apkg --root SYS:Walk --index RAM:index.json install micropolis
fetching https://github.com/tomaszstaniak/aros-micropolis/releases/download/v0.1.0-rc3/micropolis.x86_64-aros-v11.lha
  redirect -> https://release-assets.githubusercontent.com/github-production-release-asset/…
verified and cached SYS:Walk/cache/micropolis.lha
installed micropolis
1.AROS:> List SYS:Walk
micropolis.info             1822 ---rwed Today       09:38:48
micropolis                   Dir ---rwed Today       09:38:47
tmp                          Dir ---rwed Today       09:38:44
cache                        Dir ---rwed Today       09:38:44
db                           Dir ---rwed Today       09:38:48
```

The drawer is `micropolis`, named by the `id`, and its icon lies beside it.
`apkg info micropolis` prints the registry entry the client wrote. The
[reference](metadata.md#the-local-registry) describes it; it
listed 130 files here.

### 5.3 Start

Start the program the way a user would: from its icon, and from a Shell if
the ReadMe says so. The walk started it from a Shell:

```
1.AROS:> CD SYS:Walk/micropolis
1.AROS:Walk/micropolis> Run >NIL: <NIL: Micropolis CITY.CTY
```

The city window opened with the map drawn.
"Started" means it opened and ran. It does not mean every feature was tried.

### 5.4 Save something

Save the way a user would, and where a user would, including the program's
own drawer if it offers that. In the walk, S opened the save requester on
`SYS:Micropolis`, a drawer the port has built in and which does not exist
when arospkg installs the game.
The drawer field was changed by hand to `SYS:Walk/micropolis`, and the city
saved as `MyTown.cty`. The game wrote `MyTown.cty.info` beside it, and its
title bar confirmed the save. A default
like that is worth reporting to the program's author. To show
how removal treats an installed file that has changed, a line was also
added to `ReadMe.txt`:

```
1.AROS:> Echo >>SYS:Walk/micropolis/ReadMe.txt "my own note"
```

Quit the program, and leave the drawer (`CD SYS:`), before you remove it.

### 5.5 Remove, and check what stayed

```
1.AROS:> apkg --root SYS:Walk --index RAM:index.json remove micropolis
note: 1 file(s) changed locally were kept in SYS:Walk/micropolis:
      ReadMe.txt
removed micropolis
1.AROS:> List SYS:Walk/micropolis
MyTown.cty.info              844 ---rwed Today       09:40:25
MyTown.cty                 51120 ---rwed Today       09:40:25
ReadMe.txt                  2481 ---rwed Today       09:40:52
3 files - 107 blocks used
1.AROS:> apkg --root SYS:Walk --index RAM:index.json list
nothing installed
```

What removal does in 0.3:

- It deletes the installed files that are unchanged: 129 here.
- It keeps an installed file that has changed (`ReadMe.txt`) and lists it.
- It keeps files the package never installed (`MyTown.cty` and its icon),
  but does not list them. That is a known gap in the report, not in the
  removal.
- It deletes the drawer icon if it is unchanged. It keeps it if Workbench
  has rewritten it.
- It writes what it left behind to `<root>/db/doctor/`:
  `{ "package":"micropolis", …, "removed":129, "kept_modified":1, "kept_icon":"", "kept": ["ReadMe.txt" ] }`.
- It keeps the downloaded archive in `<root>/cache/`.

Since the drawer is left non-empty, a fresh `install micropolis` would now be
refused ("a directory of that name is already there"). The user has to move
it aside. Section 2 explains why this matters for new versions.

### 5.6 The checklist

Before `status = "approved"`:

- [ ] `size` and `sha256` were measured on the file downloaded from `url`
- [ ] the archive's file name marker and `abi` agree
- [ ] `requires_system` comes from the binary and was filtered as in section 3
- [ ] `tools/review.py` passes on the downloaded file (the check of the pull
      request runs it too)
- [ ] the file named by `icon` is the drawer icon you meant

When it was run on a target, as in sections 5.1 to 5.5, also:

- [ ] `show` said `native`, and every requirement was `satisfied`
- [ ] it installed from `url` itself, not only from a cached copy
- [ ] it started from its icon (and from a Shell, if the ReadMe says so)
- [ ] something the user would save survived removal, and removal left only
      what it should
- [ ] a `[[test]]` in `runtime-tests/<id>.<arch>.<abi>.toml` names the
      system, the date, the steps and the `sha256` of the archive you ran

Retain detailed test logs in your own development workspace.

## 6A. An author publishing their own archive

1. **Publish the archive at a URL that will not change**: a GitHub release
   asset, or an AROS Archives upload. Never replace the file at that URL. New
   bytes need a new URL, and a new `revision` or `version`.
2. Run sections 4 and 5 on the published file, not on your local build.
3. **Hand the manifest to the index maintainer.** There is no self-service
   submission yet: arospkg-index is maintained by hand.
4. Optionally, ship `.arospkg/manifest.toml` in the archive
   ([RFC 0001](../rfc/0001-package-format.md)). It costs nothing and the
   client ignores it. It must not disagree with the index manifest.
5. Put the source archive **beside** the package, not inside it, and give its
   location in `[source]`.

Micropolis does all of this with `scripts/package-release.sh` in its own
repository. The script builds the LHA, writes the embedded manifest, and
writes the index manifest as a draft with `status = "skeleton"`, filling in
the size and hash of the archive it has just made.

## 6B. A maintainer describing someone else's upload

The manifest is our claim about someone else's archive. Its author has not
seen it.

1. **Candidates.** `tools/import_catalogue.py` reads the AROS Archives
   catalogue and writes `index/candidates.json`, and a skeleton manifest for
   each new candidate. It never modifies an existing manifest. A skeleton
   carries `status = "skeleton"`, an empty `sha256` and
   `depends_checked = false`. The `id` it suggests comes from the file name
   and is often wrong: `python-2.5.2` and `python-2.7.18` are one program.
2. **Fetch and hash.** `tools/fetch.py <id>` downloads the chosen archive
   into `.cache/archives/<category>/` and prints its SHA-256 and size. It
   refuses a download whose size differs from the catalogue. Copy the hash
   and the size into the manifest yourself.
3. **Look inside.** Find the drawer (`subdir`) and the drawer icon (`icon`).
   Read the ReadMe: an archive that asks you to copy something into `LIBS:`,
   `Fonts:` or `C:` cannot be installed by 0.3.
   Inspect the archive layout and the libraries required by its program.
4. **ABI.** Use the file name marker. When in doubt, run it: a wrong ABI
   crashes at startup.
5. **Requirements and dependencies**, as in section 3. Set
   `depends_checked = true` only once you have looked.
6. **Review**, and test where you can, as in section 5.
   `tools/candidate_index.py OUT id…` writes a test index of candidates
   without approving anything.
7. **Approve**: set `status = "approved"` and move the file from
   `index/candidates/` in this repository to `manifests/` in arospkg-index;
   record any runtime test in `runtime-tests/`.
8. **Exclude** instead, when a package cannot be supported. An
   `excluded = "<reason>"` line records the decision, so it does not stay
   an open skeleton.

## 7. From draft to published

```
skeleton ──look, measure──▶ approved manifest ──mkindex.py──▶ index.json ──commit, push──▶ clients
(arospkg: index/candidates/)   (arospkg-index: manifests/)      (arospkg-index)
```

- There is no separate "draft" status: a draft is a skeleton. Skeletons
  stay in this repository and are never published.
- **arospkg-index is the only home of approved manifests and of the index.**
  This repository keeps the tools, the candidates, and pinned test fixtures
  (`tests/fixtures/`), which are copies frozen for tests and not the current
  catalogue.
- With both repositories checked out side by side, `python3 tools/mkindex.py`
  reads `../arospkg-index/manifests/`, drops any whose `depends` cannot be
  met in the same architecture, and writes `../arospkg-index/index-v2.json`
  (every package) and `../arospkg-index/index.json` (the packages it already
  listed, within what clients up to 0.3.2 read; see the metadata reference).
- **Publishing** means committing the manifest and the regenerated index in
  arospkg-index and pushing. Keep internal run reports outside the repository.
  Clients after 0.3.2 fetch
  `https://raw.githubusercontent.com/tomaszstaniak/arospkg-index/main/index-v2.json`
  with `apkg update`; 0.3.2 and earlier fetch `index.json` at the same place.
  `--index-url` still chooses another.
