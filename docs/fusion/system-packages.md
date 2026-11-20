<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# System packages

A package of kind `application`, `tool` or `data` is a bundle: it owns the
folder `ROOT/<name>/`, and every [common operation](operations.md) replaces
that folder as one staged tree with its before-image kept. A package of kind
`system` has no folder. Its signed file list is relative to the root itself
(`C/Copy`, `Libs/foo.library`, `Prefs/Env-Archive/SYS/x.prefs`), its files
live in directories shared with the user and with other packages, and the
engine places them one by one where they are listed. No second copy of the
tree is staged and no before-image is retained, so a system volume that cannot
hold its content twice can still be installed and upgraded.

The planner, journal, recovery, events and clients are the common ones. This
page states what differs.

## Authorization

A root places a system package only from an authority whose policy entry holds
the `system` permission:

```json
{"id":"<authority>","keys":[{"algorithm":"ed25519","public_key":"<key>"}],"permissions":["system"]}
```

`permissions` is optional, valid on `authorities` entries only, and `system`
is its one value. An authority that can sign bundles for a root cannot write
into `C/` or `Libs/` until the root's policy says so; without the permission
the plan refuses with `APKG_TRUST`. The permission is granted at
initialization or by an approved [policy plan](channels.md#changing-the-policy),
whose presentation lists it; nothing grants it on first use.

## Ownership

Ownership is per regular file, from the signed list.

- A directory is shared and never owned. A file in a shared directory that the
  list does not name is not the package's: no operation reads, replaces or
  deletes it.
- A regular file has one owner. A path that another installed system package
  lists is a conflict, with or without takeover. Two packages may list the
  same directory. A path that one package drops and another lists in the same
  plan is handed over: the first neither deletes the file nor removes the
  directory.
- A bundle is the folder `ROOT/<name>/`. A system package cannot list a path
  whose first component is the name of a bundle of the resulting root, and a
  bundle cannot be installed under a name that a system package's paths
  occupy. The comparison ignores case.
- The engine keeps names of its own in a root, which a volume root holds
  beside the system's files. A system descriptor that lists `db` or `.pkg` at
  the top, or any component beginning with `.apkg` or `.pkgold`, is not a
  valid descriptor: the signed reader and the builder refuse it.

The installed record of a system package carries a local `placement` object
beside its signed descriptor: the directories the engine created for it. It is
not publisher metadata.

## Per-file decisions

The plan compares, for every path of the old and the new signed list, the old
signed bytes, the new signed bytes and the bytes that are there now. Content
identity is the SHA-256 of the file.

| Path | Live file | Decision | Plan list |
|---|---|---|---|
| Listed, not owned before | absent | write it | `write` |
| | the new signed bytes | owned without a write | none |
| | other bytes | CONFLICT; with takeover, replace and own | `taken` |
| Listed, owned before | the new signed bytes | nothing to do | none |
| | the old signed bytes, or absent | write the new bytes | `write` |
| | other bytes, `config: preserve` | keep the live file | `keep` |
| | other bytes, ordinary file | replace with the signed bytes | `overwrite` |
| Dropped by the new list | the old signed bytes | delete | `delete` |
| | other bytes | keep; it is no longer owned | `release` |
| Listed directory | a directory | shared | none |
| | absent | create, and record as created | `directories` |
| Dropped directory | created by the package | remove if empty after the deletions | `rmdir` |

A path that holds a link, a device or the other type (a directory where a
file is listed, a file where a directory is listed) is a CONFLICT that takeover
does not lift. `config: preserve` is read from the new signed list. A system
file that is not configuration must be the signed one, so an upgrade replaces
a modified ordinary file and says so in the plan.

**Install** treats every listed path as not owned before. **Upgrade** requires
a newer version or revision from the same authority, as for bundles.
**Repair** uses the installed list as both old and new: missing and changed
ordinary files are restored, edited configuration stays.

**Takeover** is an explicit planning input (`apkg_plan_system_takeover`, the
`--takeover` flag, `system_takeover` in a client request). It is recorded in
the plan as `"takeover":"1"` and each file it replaces is listed under
`taken`, so the digest the caller approves covers both the request and its
effect.

## Plan record

A plan that changes a system package is the common `changes` plan with one
more body field, `system`: an array with one record per package.

```json
{"bytes":"8","delete":["Libs/Extras/gone.library"],"directories":[],
 "keep":["S/Startup"],"name":"aros-system","observed":"<sha256>",
 "operation":"upgrade","overwrite":["C/Copy"],"release":["Libs/old.library"],
 "rmdir":["Libs/Extras"],"taken":[],"takeover":"0","write":["C/New"]}
```

`operation` is `install`, `upgrade` or `repair`. `bytes` is the total size of
the files to write. Every list holds root-relative paths in ascending order;
sizes, modes and hashes of written files are those of the new descriptor in
`new_state`. `observed` is the SHA-256 of the live observation of every path
considered, files that need no write included.

The record is a function of the old state, the new state, the takeover input
and the live bytes. Before preparation, and again at the last boundary before
the commit decision, execution builds it again from the live root and compares
it with the approved bytes: any difference is `APKG_STALE` and nothing has
been written. The presentation (`apkg_plan_format`) lists every path with its
decision, and size, mode and SHA-256 for files to be written.

## Execution and journal

A system package adds no staging to the journal. The transaction directory
holds what every `changes` transaction holds (`plan`, `plan-sha`, `old-state`,
`new-state`, `archive-id`, `ready`, `decision`); bundle participants of the
same plan keep their staged and retained trees.

1. Preparation, `ready`, and the reversible bundle steps run as documented in
   the [journal](journal.md). Nothing of a system package is written yet. A
   failure or a cancellation up to here restores the bundles and leaves the
   root unchanged.
2. The live root is compared with the approved records, a last cancellable
   `APPLY` event is emitted, and the `decision` marker is written.
3. Directories of `directories` are created.
4. Each file of `overwrite`, `taken` and `write` is streamed from the retained
   object `<root>.apkg-objects/<sha256>` into `.apkg-placing` in its
   destination directory, hashed while it is written, flushed, given its mode,
   and put in place by the engine's replacement primitive: atomically where
   the filesystem can, otherwise through the
   [aside](journal.md#replacement-without-replaceobject). A file whose bytes
   do not match its signed hash is never put in place.
5. After every placement, each file of `delete` that still holds its old
   signed bytes is deleted, then each directory of `rmdir` is removed if it is
   empty, deepest first.
6. The root state is published last, by the common state publication, and the
   journal is archived.

Bytes come only from the object store, which holds what the verified archive
delivered (or the inline bytes of a development descriptor). The file content
is never held in memory: one 64 KiB buffer streams it.

## Recovery

Placement happens after the commit decision and has nothing to go back to, so
recovery rolls forward. From `ready` until the journal is archived the root
reads as PENDING; ordinary plans refuse with `APKG_RECOVERY_REQUIRED`.

`apkg_recover` on a decided transaction whose state is not yet the new one
runs steps 3 to 6 again. Every step is idempotent:

- a destination that already holds the new signed bytes is done; a leftover
  aside of an interrupted replacement beside it is removed;
- a leftover `.apkg-placing` is replaced by the next attempt;
- a file to delete that no longer holds its old signed bytes stays;
- a directory that is not empty, or already gone, stays.

The state is published only after all of them, so a root never reads as READY
with a mix of old and new system files. A transaction interrupted before the
decision is restored as any other: the system package had not been touched.
Recovery needs the object store; while an object it has to place is missing it
returns `APKG_RECOVERY_REQUIRED` and the root stays pending. It needs no
network.

Fault checkpoints of the test build, beside those of the
[journal](journal.md): 201 directories created, 202 temporary file written,
203 file replaced, 204 all placements done, 205 after a deletion, 206 all
system steps done before state publication. `apkg_test_fault_after` lets a
number of passes through a checkpoint go by first, to stop in the middle of a
list. The tests stop the process at each of them and at each aside step, and
interrupt recovery again.

## Events

During `APKG_PHASE_APPLY` the observer receives progress with a known total:
`bytes` events with the bytes placed so far out of the plan's total, at least
once per file and once per MiB within a file, and a `files` event after each
file. The subject is the root-relative path. These events are not cancellable.
The last cancellable event is the `APPLY` event emitted before the decision; a
cancellation requested later is recorded in the result and the transaction
still commits. Recovery of an interrupted placement reports the same progress
under `APKG_PHASE_RECOVER`, counting the files an earlier attempt finished.

## Verify

`apkg verify ROOT NAME` (`apkg_client_verify`) reports each listed path:

| Row | Meaning |
|---|---|
| `ok` | the signed bytes, or a listed directory that is a directory |
| `missing` | nothing at the path |
| `changed` | an ordinary file with other bytes, or the wrong type |
| `edited` | a `config: preserve` file with other bytes: a personal edit, not damage |

Totals precede the rows. Files the list does not name are not reported.

## Not supported

These refuse with `APKG_UNSUPPORTED` and an error that names the package:

- **remove**, including removal through another package's `replaces`;
- **rollback** and **rollback-last** of a transaction that placed system
  files, and of a mixed transaction as a whole;
- **adoption** through the bundle `adopt` and `adopt-set` paths;
- turning an installed bundle into a system package of the same name, or the
  reverse;
- `amiga` metadata on an entry of a system package (protection bits beyond the
  POSIX mode, comments, owners). The builder refuses a system drawer that
  carries `.ameta` controls.

File identity is the content hash. An owned file with the signed bytes and a
different mode is not rewritten; the mode is applied when a file is written.
Timestamps are not preserved. Ownership checks between two system packages
compare exact paths.

## Clients

`install`, `upgrade`, `resolve`, `repair`, `channel-install` and
`source-install` handle a system package as any other; all but `repair` accept
`--takeover`. `apkg_client_request_apply` is the library form with the
takeover input.

`apkg_client_channel_update` is the call for an unattended updater:

```text
apkg channel-update ROOT ENDPOINT CHANNEL-ID NAME --policy FILE
                    [--target TARGET] [--takeover] [--yes | --confirm DIGEST]
```

It recovers an interrupted transaction first, initializes an absent root with
the pinned policy when a target is given, refuses with `APKG_POLICY` when a
ready root's policy is not byte for byte the pinned one, then installs or
upgrades the newest trusted candidate of `NAME` from the endpoint, a local
channel directory or an `http(s)` URL. A current package returns an UNCHANGED
result. The library call takes an observer, a result, the takeover flag and
explicit budgets; without a confirm callback it approves each plan as made.

## Authoring

A TOML with `kind = "system"` builds a system package: the drawer's content
maps to the root itself, so `subdir`, `icon` and `launch` do not apply and no
provenance copy of the TOML is added to the payload. `config` names files
relative to the drawer. See [authoring](authoring.md#system-packages).

## Limits

| Bound | Value |
|---|---|
| Descriptor, state record and plan record | each within `min(max_object_bytes, max_memory_bytes / 32)`: 2 MiB with default budgets, about 8,000 listed files; up to the 16 MiB object ceiling with `max_memory_bytes` raised |
| Tokens of one canonical object | 1,048,576 |
| Channel snapshot (`channel.json`) | the context's object limit, 16 MiB by default; it embeds the full descriptor of every retained build, about 250 bytes and 13 tokens per listed file |
| Payload of one package | 2,147,483,647 bytes (PKG/1) |
| Path | 4,095 bytes, 32 components, printable ASCII |

Planning, execution and verification walk the sorted lists in step and sort
once where they index: no step is quadratic in the number of files. A
4,000-file package plans, installs, verifies and upgrades within the default
budgets.

Each of planning and the two live checks of execution hashes every listed file
that exists; execution reads each object once to place it.

### Storage beside the root

The engine keeps its verified objects in `<root>.apkg-objects`, the fetched
archive in `<root>.apkg-archives`, and the journal and its history beside the
root. For a root that is a volume (`SYS:`), these names are inside the volume.
Placement itself needs no second copy, but the retained objects are one, and a
fetched archive is another until it is removed. Nothing in the engine prunes
either store. A volume that cannot hold its system twice therefore needs these
stores elsewhere or pruned, which the engine does not provide yet; see
[status](status.md#system-packages).

Host tests establish the behavior above, process death included. Power-loss
durability and the AROS handlers require evidence on the target.
