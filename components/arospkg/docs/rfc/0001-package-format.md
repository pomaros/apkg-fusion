---
title: "RFC 0001: package format and embedded manifest"
status: draft
schema: 0
created: 2026-09-06
updated: 2026-09-29
replaces: the earlier package-standard proposal (retained in Git history)
---

# RFC 0001: package format and embedded manifest

| | |
|---|---|
| **Status** | Draft. **Not implemented**: no released client reads an embedded manifest. |
| **Schema version** | `0`: draft, may change incompatibly. |
| **Companion** | [RFC 0002](0002-application-folder.md), the application folder format |
| **Current format** | [metadata reference](../guide/metadata.md): what the client reads today |
| **Notes for the user** | `post_install_notes` is supported today in the **index manifest** (see the metadata reference). An embedded manifest may later carry the same field under the same rules; this draft does not add it. |

Each field below has one of three statuses:

- **supported**: the index manifest already has the same field with the same
  meaning, and the client uses it. What is new is only that the author states
  it inside the archive.
- **proposed**: defined here and used by nothing yet.
- **deferred**: named so the name is kept, but not defined by schema 0. A
  reader must not act on it.

## 1. Goal

Let the author of a program describe their own release once, inside the
archive, so that an index entry can be checked against it, or derived from
it, instead of being written from scratch by someone else. Today every
package needs a manifest written by the index maintainer, and much of it is
information the author already has.

## 2. Scope

In scope: the location and syntax of the manifest; the package's identity,
targets, the part of the archive that is installed, the drawer icon, system
requirements, provenance; how the manifest relates to the index manifest and
to the client's registry.

Out of scope for schema 0: download location, size and archive hash (they
belong to the index, since a file cannot contain its own hash); installation
scripts of any kind; dependencies between packages, conflicts, file roles and
writes outside the package's drawer (deferred, section 7); signing.

## 3. The archive

An ordinary ZIP or LHA archive, in the formats and within the limits the
[reference](../guide/metadata.md) and the
[guide](../guide/packaging.md#1-the-archive) give. No new container.

```
micropolis.x86_64-aros-v11.lha
├── Micropolis.info           drawer icon            install.icon
├── Micropolis/               the installed part     install.subdir
│   ├── Micropolis            program
│   ├── Micropolis.info       program icon
│   └── …
└── .arospkg/manifest.toml    this manifest
```

The manifest is `.arospkg/manifest.toml`, at the top level of the archive,
in UTF-8 TOML 1.0, and at most 64 KiB (proposed). `.arospkg/` is never
installed.

## 4. A complete example

The manifest shipped in the Micropolis 0.1.0-rc3 release archive, unchanged.
It is a real schema-0 manifest, which no client reads yet.

<!-- proposal -->
```toml
schema   = 0
id       = "micropolis"
name     = "Micropolis"
version  = "0.1.0-rc3"
revision = 1
summary  = "Classic city-building simulation for AROS"
category = "game/strategy"
license  = "GPL-3.0-or-later with EA Section 7 terms; Micropolis Public Name License"

depends = []
conflicts = []

[[requires_system]]
type = "library"
id   = "crt.library"

[[requires_system]]
type = "library"
id   = "m.library"

[[requires_system]]
type = "library"
id   = "stdlib.library"

[[targets]]
os   = "aros"
arch = "x86_64"
abi  = "v11"

[install]
subdir = "Micropolis"
icon   = "Micropolis.info"

[source]
archive    = "Micropolis-AROS-0.1.0-rc3-source.zip"
sha256     = "969be5094991e8c57b9689da06e68c4082daf4491a907411db0972aa3635e5e5"
revision   = "745bf682d12eec76b1924fd5daaa33895df1f6de"
built_with = "AROS One ABIv11 x86_64 SDK / GCC 10.5.0"
```

The GrafX2 2.9-r4 and Folio 0.3.8 release archives carry manifests of the
same shape.

## 5. Fields

| field | type | required | status | definition |
|---|---|---|---|---|
| `schema` | integer | yes | proposed | `0` for this draft. A reader that does not support the value must ignore the whole manifest (section 6). |
| `id` | string | yes | supported | As in the reference, but at most 63 characters, which is what the client keeps: `^[a-z][a-z0-9._-]{0,62}$`. Stable; it is the installed drawer's name. |
| `name` | string | yes | proposed | The name people see, such as `"Micropolis"`. Free text, one line. |
| `version` | string | yes | supported | Upstream's version string, opaque, compared for equality only. |
| `revision` | integer ≥ 1 | no | supported | This port's release count for `version`. Absent: no separate port release. |
| `summary` | string | no | supported | One line. |
| `category` | string | no | supported | AROS Archives style path. |
| `license` | string | no | supported | Free text; an SPDX expression where one fits. Not checked. |
| `[[targets]]` | array of tables | yes, at least one | proposed shape | What the archive's binaries run on. Each entry has all three keys: |
| `targets[].os` | string | yes | proposed | `"aros"`. |
| `targets[].arch` | string | yes | supported | `"x86_64"` or `"aarch64"`. |
| `targets[].abi` | string | yes | supported | `"v1"` or `"v11"`. |
| `[install]` | table | yes | | |
| `install.subdir` | string | yes | supported | The directory at the top level that is installed, as `<root>/<id>/`. `""` means the whole top level **except `.arospkg/`**; the 0.3 client does not make that exception yet. |
| `install.icon` | string | no | supported | The drawer icon: a `.info` file at the top level, beside `subdir`. |
| `[[requires_system]]` | array of tables | no | supported | Keys `type`, `id`, `min_version`, as in the reference. Omitting it states that no requirements are declared, which is not a proof that there are none. |
| `[source]` | table | no | supported | Keys `repository`, `revision`, `archive`, `sha256`, `built_with`, `built_on`, as in the reference. |
| `depends` | array | no | deferred | See section 7. `[]` is allowed and means nothing. |
| `conflicts` | array | no | deferred | See section 7. `[]` is allowed and means nothing. |
| `[[files]]` | array of tables | no | deferred | See section 7. |
| `[[install.external]]` | array of tables | no | deferred | See section 7. |

A manifest carries no `url`, `size`, `sha256` of its own archive, `status`
or test claims. Those describe a copy of the archive, or a review of it, not
the package itself.

## 6. Interpretation and errors

A reader is any tool that reads the manifest: an index generator, a
client, a file manager.

1. **Schema.** A missing `schema`, or one the reader does not support, means
   the reader ignores the manifest. It must not guess from a manifest it
   does not understand.
2. **Syntax.** A file that is not valid TOML, including one with a duplicate
   key, is invalid.
3. **Required fields and types.** A missing required field or a value of the
   wrong type makes the manifest invalid. A generator refuses it. A client
   does not install from it.
4. **Unknown keys.** Ignored by readers, and reported by generators as a
   warning. (Open: O8.)
5. **Paths** (`install.subdir`, `install.icon`, and any future path) are
   relative and use `/`. A path is invalid if it is empty where a name is
   required, or contains `:`, `\`, a `..` component or a control character.
   `subdir` names one top-level directory. `icon` names one top-level file
   ending in `.info`. A path that does not exist in the archive is an error.
6. **Targets.** A client installs only when one target matches its own
   `os`, `arch` and `abi` exactly. An unknown `abi` or `os` is
   *undetermined*, and an undetermined target is not a match. Several matching
   targets in one archive are fine, since they describe the same files.
7. **Requirements** are probed as in the reference: *missing* refuses,
   *undetermined* installs with a warning and is recorded.
8. **No code runs.** Reading a manifest never executes anything, and the
   format has no hooks, scripts or commands.

## 7. Deferred

These names are reserved, and their meaning is not settled:

- **`depends`, `conflicts`**: dependencies between packages, and declared
  incompatibilities. The index manifest has `depends` as a list of ids, which
  `mkindex.py` checks and the 0.3 client does not read. The older draft used
  tables with `min_revision`. Schema 0 allows only `[]`.
- **`[[files]]`** with `kind = program | config | userdata`: which files the
  user owns after installation. Creating an empty data directory, shipping
  seed data and owning files the program writes later are three different
  things. A declaration must not give permission to delete what it names.
- **`[[install.external]]`**: files placed outside the drawer, such as `LIBS:`
  and `Fonts:`. This needs rules for shared files, reference counting and
  conflicts before any client may do it. The 0.3 client never writes outside
  the package root.

## 8. The embedded manifest, the index manifest and the registry

Every piece of data has **one** authority:

| data | authority | the others |
|---|---|---|
| `id`, `version`, `revision`, targets (`arch`, `abi`), `subdir`, `icon`, `license`, `[source]`, `summary`, `category` | the **embedded manifest**, when there is one | the index manifest repeats them; they must be equal |
| `requires_system` | the **embedded manifest** | the index manifest may add entries found in review (Open: O2) |
| `name` | the embedded manifest | the index has no such field yet |
| `url`, `size`, `sha256`, `status`, `depends_checked`, `installs_on`, `runs_on`, `does_not_run_on`, review notes | the **index manifest** | not in the embedded manifest |
| what was installed, from where, with which hashes and verdicts | the **registry** | written by the client only |

Rules (proposed; nothing enforces them yet):

1. A generator that finds an embedded manifest compares every field in the
   first row with the index manifest, and refuses the entry if any differs.
   A disagreement means one of the two is wrong, and a person decides which.
2. An archive without an embedded manifest is described by the index
   manifest alone, as today. That is the normal case for existing uploads.
3. The client keeps reading only the index. It never merges data from the
   archive into what it shows before the download.
4. A new release needs a new archive. Changing any field in the first row
   means a new `revision` or `version`.

Today this rule is followed by hand. The Micropolis, GrafX2 and Folio index
manifests say in a comment that they agree with the archive. Folio 0.3.8's
embedded manifest declared no requirements, and its index manifest added
three.

## 9. Open decisions

- **O1. Derive or compare?** Should the generator build the index manifest
  from the embedded one plus review data, instead of comparing two files?
  That would leave one copy of each field instead of two.
- **O2. Requirements added in review.** May the index add requirements that
  the author did not declare (proposed: add, never remove)? Or must the
  author publish a new revision?
- **O3. Ordering.** Nothing orders one `version` against another. It is also
  open whether "no `revision`" sorts before `1` (the 0.3 client treats it as
  0).
- **O4. Dependencies and conflicts**: syntax, and what a version constraint
  can mean without an ordering.
- **O5. File roles**, section 7.
- **O6. External destinations**, section 7.
- **O7. Target vocabulary.** Values of `os`. Whether mainline keeps a stable
  ABI tag. How to describe a 32-bit ABIv0 program run through an emulator:
  its target stays `i386`/`v0`, and running it is a separate fact.
- **O8. Unknown keys and extensions.** Warn and ignore, or a namespace for
  experimental keys?
- **O9. Several targets with different files.** Schema 0 lets several targets
  share one `subdir`. Different binaries per target would need a `subdir`
  per target.
- **O10. Signing** is a separate document. A hash in an index proves that a
  file matches the index, not who wrote the index.
