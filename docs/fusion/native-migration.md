<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Tomasz Staniak -->

# Native migration: inspection and copy

`apkg migrate` is the part of migrating a root of the classic client
(arospkg 0.1 to 0.7.8) that runs on AROS itself. This page covers the first
half, which is implemented: looking at the classic root and making a checked
copy of it. Registering the copied packages with the common engine is the
second half and is not implemented; see [Open](#open).

The [host adapter](legacy-copy.md) remains the rehearsal over signed
artifacts. The native command needs no artifact, mapping or policy.

## Commands

```text
apkg migrate check ROOT [--json]
apkg migrate copy ROOT NEW-DRAWER --quiescent [--json]
apkg init NEW-DRAWER TARGET POLICY-FILE --yes
apkg migrate register NEW-DRAWER [--json | --confirm DIGEST]
```

Both accept `--legacy-envarc DRAWER` and `--legacy-env DRAWER`, copies of
`ENVARC:arospkg` and `ENV:arospkg`. On AROS the two drawers themselves are
read when no copy is named; on a host they are reported as not inspected.

`check` reads the root and changes nothing. For each installed record it
reports version, revision, CPU, ABI and installation reason, how the files in
the package drawer compare with the recorded baseline (unchanged, changed,
missing, and files the record does not name), and what became of the drawer
icon at `<root>/<id>.info` (as delivered, changed, missing, hash not recorded,
or the user's own icon kept at installation).

`copy` does the same inspection, then copies the whole root into a drawer
that must not exist, in a drawer that must. The original is only read.
`--quiescent` is the operator's statement that no classic client (`apkg`,
PkgManager) runs meanwhile. Inside the copy the classic registry moves from
`db` to `.apkg-legacy/db`, so a classic client no longer takes the copy for
its root, and `.apkg-legacy/inspection.json` records what was found. The
command prints the SHA-256 of that record. The copy is not a common root yet.

`init` is the ordinary command; it makes the copy a common root for the named
target with the named policy.

`register` reads the records under `.apkg-legacy/db/installed` and registers
every package as an [unsigned entry](data-model.md#unsigned-entries-of-a-migrated-installation).
It writes the adoption set to `.apkg-legacy/adoption-set.json` and hands it to
the same plan, preview and confirmation as `apkg adopt-set`: without
`--confirm` it prints the plan and its digest and returns 16; with the digest
it commits. Each record must be for the root's target, its drawer must be named
after the package, and its version must be one the common engine accepts;
otherwise nothing is registered. Files whose bytes or mode differ from the
record are kept as the user's; files the record does not name are unowned.
Recorded dependencies between the migrated packages are kept, so removing a
needed package is still refused. Recorded conflicts, replacements,
recommendations and system requirements stay in the evidence only.

A record with launch data gives kind `application`. The named program must be
a valid path inside the drawer. Where the record and the drawer's
`application.json` name a different program or mode, `register` prints both
and chooses neither: the record stays in the evidence, the file stays in the
drawer. Nothing is run.

## What is checked

The root is refused, before anything is copied, when it holds:

- `.pkg`, `.apkg` or `.apkg-legacy` (mixed or already converted);
- `db/lock.owner`, an entry in `db/transactions`, or `db/replace.json`;
- a `self-update.json` anywhere in the root;
- in `ENVARC:arospkg`: `self-update.json` or a `.new` file; in `ENV:arospkg`:
  `self-update.lock`, the `self-update.<nonce>` file the lock is taken
  through, or a `.new` file;
- no installed record, a record that is not schema 1, has fields this reader
  does not know, lacks a file baseline, or whose name, file name and drawer
  do not agree;
- a recorded dependency that is not installed, or a recorded conflict or
  replacement that names an installed package;
- a soft link, hard link or special file, a name outside the engine's path
  alphabet (ASCII without `\`, `:` and `"`), or nesting deeper than 32. The
  refusal names the entry.

Records are read with a reader for JSON as the classic client writes it
(whitespace, numbers, any key order, duplicate keys refused); the engine's own
reader accepts canonical documents only.

The root is read three times and written once. The first pass hashes every
file and compares package drawers with their records. The second pass copies
and hashes what it reads; its inventory digest must equal the first, which
shows that the bytes copied are the bytes inspected. The third pass reads the
copy back and must give the same digest again. The digest covers type, mode,
size, content hash, Amiga attributes and path of every node. Protection bits
and comments are written through the engine's attribute code; file dates are
kept where the filesystem allows and are not part of the digest. An
interrupted or refused copy is left in place and named, and is never reused.

The inventory is a running digest, not a document, so the size of a root is
not bound by the engine's 100,000-token document limit. That limit does bind
the engine's own tree reader: `apkg source-inventory` over a drawer of 9,000
files refuses, one of 7,000 files passes. A package drawer above about 7,690
entries therefore cannot be adopted, installed or repaired by the common
engine today, whatever the migration does.

## Inspection record

`.apkg-legacy/inspection.json` is one canonical JSON document, format
`apkg-legacy-inspection-1`: source and destination paths, the inventory
digest with file, drawer and byte counts, the state of the classic client's
two drawers, the operator's quiescence statement, and one row per package
with the facts `check` prints, the SHA-256 of the original record bytes and
`"authentication":"none-recorded"`. The original records stay in
`.apkg-legacy/db` byte for byte. Paths in the record are local; keep real
records out of public source control.

## Results

Host: `tests/apkg/test_migrate_native.py`, 12 tests over the roots released
arospkg 0.7.4 wrote on AROS (`tests/fixtures/migration/captured-0.7.4-v1`),
including a root of 9,000 added files.

AROS, mainline x86_64 nightly 20261113 (`dos.library` 50.89), 2026-11-16, the
same captured root unpacked in `RAM:` and copied to the SFS system volume:
`check` and `copy` return 0 on both volumes; the copy lists the same files
with the same sizes, protection bits (`s` set, `w` cleared), file comment and
file dates as the original; the registry is under `.apkg-legacy/db`. Refusals
returned 17 (no `--quiescent`), 14 (destination exists), 1 (destination
inside the root), 17 (the copy as a source), 7 (`db/lock.owner`), 9 (the
captured interrupted install) and 7 (`ENV:arospkg/self-update.lock`).

Not run: ABIv11, the Pi, a root written by 0.7.5 to 0.7.8, a root with many
large packages, drawer comments, a full volume, power loss during the copy.

Registration, host only: `tests/apkg/test_migrate_register.py`, 19 tests over
the same captured root (form of the entries, queries, verify, refused repair,
refused replacement by a signed package of the same name, refused policy and
signature for the reserved authority, invalid state forms, removal keeping
the user's files). `register` has not been run on AROS yet.

## Open

These need both maintainers and are proposals until agreed:

- **Takeover** of an unsigned entry by a signed package of the same name: an
  explicit operation whose plan shows the change of authority and needs its
  own approval. Until it exists such a package is refused. Dependencies
  between migrated packages name the reserved authority, so a takeover has
  to say what becomes of them.
- **Protection bits and comments** of migrated files: the unsigned descriptor
  carries no AROS attributes. A file with a comment or unusual protection
  is then expected to count as changed by the user and stay on removal; this
  has not been checked on AROS.
- **Owned placement of the drawer icon** when its bytes are the delivered
  ones. The inspection already reports which icons qualify.
- **Sets above 128 packages**: the limit of `apkg_plan_adopt_many` belongs to
  the importer, which has to split or raise it.
- **The 7,690-entry limit** of the engine's tree reader, above.
- **Switching a system to the copy**: assigns, `C:apkg` copies listed in
  `ENVARC:arospkg/copies.json`, and what happens to the original root.
