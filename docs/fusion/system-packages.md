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
- AROS volumes do not tell case apart, so ownership does not either. Paths are
  compared by their case-folded form everywhere: between the old and the new
  list of one package, between packages, and against bundle folders. `C/copy`
  renamed to `C/Copy` in a new version is the same path, never a file to
  delete and another to create. A descriptor that lists two paths folding
  alike is not a valid descriptor.
- A bundle is the folder `ROOT/<name>/`. A system package cannot list a path
  whose first component is the name of a bundle of the resulting root, and a
  bundle cannot be installed under a name that a system package's paths
  occupy.
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
| | other bytes | CONFLICT without takeover | |
| | other bytes, takeover, `config: preserve` | keep the live file; it is owned from now on | `keep` |
| | other bytes, takeover, ordinary file | replace and own | `taken` |
| Listed, owned before | the new signed bytes | nothing to do | none |
| | the old signed bytes, or absent | write the new bytes | `write` |
| | other bytes, `config: preserve` | keep the live file | `keep` |
| | other bytes, ordinary file | replace with the signed bytes | `overwrite` |
| Dropped by the new list | the old signed bytes | delete | `delete` |
| | other bytes | keep; it is no longer owned | `release` |
| Listed directory | a directory | shared | none |
| | absent | create, and record as created | `directories` |
| Dropped directory | created by the package | remove if empty after the deletions | `rmdir` |

`config: preserve` is read from the new signed list. A system file that is not
configuration must be the signed one, so an upgrade replaces a modified
ordinary file and says so in the plan. Takeover never replaces a preserved
configuration file: the first adoption of a volume a person has already
configured keeps their settings and reports each one under `keep`.

A path may change type between two versions when what stands there is the old
version's own, unmodified:

| Old list | New list | Accepted when | Plan |
|---|---|---|---|
| file | directory | the live file holds the old signed bytes | `clear` the file, then `directories` |
| directory | file | everything under the live directory is listed by the old version, and every such file holds its old signed bytes | `clear` the children and the directory, then `write` |

Anything unlisted or modified in the way is a CONFLICT that takeover does not
lift, as is a link, a device, or a file or directory where the package never
listed the other type.

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
{"bytes":"8","clear":[],"delete":["Libs/Extras/gone.library"],"directories":[],
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
2. The last checks, after which nothing can be taken back:
   - the live root is compared with the approved records;
   - every object to place is read from the store and hashed: a missing or
     altered object refuses with `APKG_NOT_FOUND` or `APKG_INTEGRITY`;
   - the destination volume must have room: `APKG_LIMIT` otherwise. An
     in-place replacement needs the new file beside the old one until it takes
     the name, so the room counted is the highest point of the placement
     sequence (each new file in full, each replaced file's size given back
     once it is replaced), plus one allocation unit per file and directory,
     twice the size of the state record, and 256 KiB;
   - nothing holds what will be replaced or removed, and the root is not the
     running system on a DOS without a one-step replacement: see
     [objects in use](#objects-in-use).

   Each refusal names the path or the bytes needed and available, and leaves
   the root READY with nothing written. A last cancellable `APPLY` event is
   emitted, then the `decision` marker is written.
3. Paths of `clear` are removed, children before their directory, each only
   while it still is what the old version left.
4. Directories of `directories` are created with owner write, whatever their
   signed mode says.
5. Each file of `overwrite`, `taken` and `write` is streamed from the store
   into `.apkg-placing` in its destination directory, hashed while it is
   written, flushed, given its mode, and put in place by the engine's
   replacement primitive. A file whose bytes do not match its signed hash is
   never put in place.
6. After every placement, each file of `delete` that still holds its old
   signed bytes is deleted, then each directory of `rmdir` is removed if it is
   empty, deepest first.
7. Created directories receive their signed modes. A directory that had no
   owner write and had to be opened for a change gets its mode back.
8. The root state is published last, by the common state publication, and the
   journal is archived. The [store is pruned](#store-and-pruning).

Bytes come only from the store, which holds what the verified archive
delivered (or the inline bytes of a development descriptor). The file content
is never held in memory: one 64 KiB buffer streams it.

### Edits made after the approval

A file of `write` is replaced only if it is absent or still holds the old
signed bytes. If it holds anything else when its turn comes, someone edited or
created it after the plan was approved, most likely while an interrupted
transaction waited for recovery: it stays as it is, and the transaction's
journal records its path in a `kept` file that is archived with the history.
Files of `overwrite` and `taken` are replaced regardless: the approved plan
said so. A kept ordinary file shows as `changed` in `verify`, and `repair`
restores it; a kept configuration file shows as `edited`.

### Objects in use

DOS does not delete a file that is open or locked. On the
[aside path](#replacement-without-an-atomic-primitive) the new file takes the
name and the old bytes, still open, cannot be deleted from their aside; a file
of `delete` cannot be deleted at all. Both would happen after the decision,
so the engine asks first.

Each existing file of `overwrite`, `taken`, `write`, `delete` and `clear`, and
each directory of `clear` and `rmdir`, is asked for with an exclusive lock that
is released at once (the probe of the [bundle check](journal.md#a-live-tree-in-use),
on the object alone). The check runs twice: after the stale-plan checks,
before the private draft exists, and again among the last checks before the
decision. A held object refuses with `APKG_BUSY` and its root-relative path as
the subject; nothing was written, and after the second check the bundle
participants of the same plan are restored through the journal. An answer
other than success, `ERROR_OBJECT_IN_USE` or `ERROR_OBJECT_NOT_FOUND` refuses
with `APKG_IO` and the path. A directory that the plan only writes into is not
asked for: the `PROGDIR:` of a running program or an assign on `C/` does not
block a file placed there.

The check is a moment, not a hold. A holder that appears after it is met
during placement: the step that cannot delete returns `APKG_BUSY`, the result
is NEEDS_RECOVERY with an error that names the path, and the root stays
pending with the files placed so far in place. `apkg_recover` returns
`APKG_RECOVERY_REQUIRED` with the same `APKG_BUSY` error for as long as the
object is held, and finishes the transaction once it is free. A held directory
of `rmdir` simply stays, as a directory that is not empty does.

Passing these probes does not make the running system safe to update. Where a
replacement is not one step, a file is absent under its name between two
renames whether or not anybody holds it. A plan with a system record therefore
refuses with `APKG_UNSUPPORTED` when its root is the `SYS:` of the running
system and DOS has no `ReplaceObject` (the build has no declaration of it, or
`dos.library` is older than 50.84). The test is the root itself: a root in a
drawer of the system volume is not the running system. Where DOS has
`ReplaceObject`, whether the handler implements it is known only when a
replacement is tried, and that case is not refused.

A host has no such lock. Fault points of the test build stand for the answers:
1304 and 1305 for a held object at the first and the second check, 1306 for a
root that is the running system, 210 for an aside that cannot be deleted, 211
for a file of `delete` that cannot be deleted; `pkg_fs_test_hold` makes the
aside replacement itself fail at its last step.

Run on 2026-11-26 with a test build of the CLI that can arm a fault, a signed
two-version package from a file channel and a second process holding a file
through `Open()` or a drawer through an assign:

| System | Volumes |
|---|---|
| AROS x86_64 mainline, `dos.library` 50.89 | RAM:, SFS |
| AROS x86_64 ABIv11 (AROS One), `dos.library` 50.77 | RAM:, SFS |
| AROS Raspberry Pi AArch64, `dos.library` 50.89 | RAM: |

On each: a held file of `write` and a held drawer of `rmdir` refuse with
`APKG_BUSY` and the path, and the root verifies as before; an update stopped
after its first replaced file leaves a pending root; recovery with the next
file held stops with the `APKG_BUSY` error, the new file under its name and
the old bytes under the aside name; recovery with a file of `delete` held
stops the same way; recovery with nothing held commits, and the root verifies
with no aside or temporary file left. A replacement stopped between its two
renames recovers too. On mainline x86_64 a plan for the root `SYS:` was
refused as the running system. All of these builds take the aside path:
`ReplaceObject` was not exercised.

## Recovery

Placement happens after the commit decision and has nothing to go back to, so
recovery rolls forward. From `ready` until the journal is archived the root
reads as PENDING; ordinary plans refuse with `APKG_RECOVERY_REQUIRED`.

`apkg_recover` on a decided transaction whose state is not yet the new one
runs steps 3 to 8 again. Every step is idempotent:

- a path of `clear` that already has the new type is done;
- a directory that exists is opened for writing and gets its mode at the end;
- a destination that already holds the new signed bytes is done; a leftover
  aside of an interrupted replacement beside it is removed;
- a destination that is absent while its old bytes wait under the aside name
  is a replacement stopped between its two renames: placing the new file
  finishes it and removes the aside;
- a leftover `.apkg-placing` is replaced by the next attempt;
- a file to delete that no longer holds its old signed bytes stays;
- a directory that is not empty, or already gone, stays.

The state is published only after all of them, so a root never reads as READY
with a mix of old and new system files. A transaction interrupted before the
decision is restored as any other: the system package had not been touched.

Recovery needs the store and no network. While an object it has to place is
missing or altered it returns `APKG_RECOVERY_REQUIRED` and the root stays
pending, with what was placed so far in place. The updater call
([clients](#clients)) then fetches the archive of that same build again,
admits its objects under the digests the approved plan lists, and finishes the
transaction. One case needs a person: a path of `clear` that was edited while
the transaction was pending blocks the directory or file that must take its
place, and recovery will not destroy it.

Fault checkpoints of the test build, beside those of the
[journal](journal.md): 201 directories created, 202 temporary file written,
203 file replaced, 204 all placements done, 205 after a deletion, 206 all
system steps done before state publication, 207 after a cleared path, 208
within the write of a temporary file, 209 between the deletions and the
directory removals. `apkg_test_fault_after` lets a number of passes through a
checkpoint go by first, to stop in the middle of a list. The tests stop the
process at each of them and at each aside step, during execution and again
during recovery.

### Replacement without an atomic primitive

Where DOS offers `ReplaceObject` and the handler implements it, a file is
replaced in one step. Elsewhere the engine uses the
[aside](journal.md#replacement-without-replaceobject): the old file moves to
`.pkgold<hash>`, the new one takes the name, the aside goes. Between the two
renames **the file does not exist under its name**. Recovery finishes such a
replacement, but a program that opens the file in that window does not find
it, and a system that boots from the volume in that state may not start.

A system root on such a handler is therefore updated from another running
system: the RAM image the updater boots, not the installed system it
replaces. An interruption then costs only a second run of the updater, which
recovers before it does anything else. Updating the system a machine is
running from is not supported without `ReplaceObject`, and the engine
[refuses it](#objects-in-use) where DOS does not have the call.

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

## Store and pruning

The engine keeps the verified objects of a root and the archives it fetched
in a store. By default the store is beside the root: `<root>.apkg-objects` and
`<root>.apkg-archives`. For a root that is a volume (`SYS:`), "beside" is
inside the volume, which a small system volume cannot afford.

`apkg_open_options.store` names an existing directory instead, for instance
on another volume: objects then live in `DIR/objects` and archives in
`DIR/archives`. The CLI takes `--store DIR` on every command;
`apkg_client_store` sets it for the client calls of a process, and
`apkg_client_request` and `apkg_client_update` take it per call. State,
journal and history always stay with the root. With a store elsewhere, an
install creates nothing inside the root but `.apkg/state` and the system's own
files, and nothing beside it but the journal.

An external store belongs to one root. The first import binds it to the canonical root path. Use a fresh directory for each root. An existing store with objects and no ownership record can still supply reads.

The root does not record where its store is. The choice belongs to the
caller, like the memory budgets, and nothing depends on it for correctness: a
context opened without the store looks in the default place, finds no object,
and every operation that needs bytes refuses cleanly. Repair then says
`the store holds no bytes for this file` and changes nothing; recovery keeps
the root pending. The same caller with the right store, or the updater with a
channel to fetch from, proceeds.

Nothing but pruning removes from a store. `apkg prune ROOT`
(`apkg_store_prune`) removes every object that no installed descriptor of the
committed state references, every fetched archive and every unfinished
transfer file; what repair of the installed versions needs stays. It runs
under the mutation lock, refuses with `APKG_RECOVERY_REQUIRED` on a pending
root so that a transaction never loses what it was approved with, and touches
only regular files of the two store directories that carry the names the
engine gives them. An interruption leaves a store that is merely less pruned.

A committed system install, upgrade or repair prunes by itself, during
execution or the recovery that finishes it. Nothing is lost by that: a system
transaction cannot be rolled back, and no earlier transaction can be rolled
back once a later one has committed. An explicit prune after a bundle
transaction removes the bytes its rollback would need; that rollback then
refuses with `APKG_NOT_FOUND`.

## Clients

`install`, `upgrade`, `resolve`, `repair`, `channel-install` and
`source-install` handle a system package as any other; all but `repair` accept
`--takeover`. `apkg_client_request_apply` is the library form with the
takeover input and the store.

### Fetch, then apply

A system updater applies at boot, from a RAM image, before any network
exists. The work is split in two.

**While the installed system runs**, with the network up:

```text
apkg channel-mirror ROOT ENDPOINT CHANNEL-ID NAME DIRECTORY
```

`apkg_client_channel_mirror` opens the channel at `ENDPOINT`, an `http(s)` URL
or a directory, under the root's policy and replay floor. The root is opened
read-only: nothing in it or beside it changes. It selects the newest build of
`NAME` whose author the policy trusts and whose target is the root's, and
writes into `DIRECTORY` a [file channel](channels.md#verified-mirror): the
signed snapshot exactly as served and that build's archive. Each file is
written under a transfer name and takes its name only once whole, the archive
only with the size and SHA-256 the signed snapshot states; `current` is
replaced last, in one step. An interrupted mirror is therefore the previous
complete mirror, or no mirror. A transfer does not resume: an unfinished or
damaged archive is fetched again whole. Archives and snapshots the mirror no
longer references are removed after `current` is switched.

The command prints, and the call returns, the snapshot digest and sequence,
the selected version and revision, the archive digest and size, and:

| `update` | Meaning |
|---|---|
| `ready` | the build is newer than the installed one, or none is installed |
| `current` | the build is the installed one: nothing to do |
| `none` | the installed build is not older than anything the channel offers |

Use an empty destination for the first mirror. A successful claim creates
the directory `.apkg-mirror-v1`; an interrupted transfer can resume there.
Concurrent publishers serialize changes to the mirror. A source repository
and an unrelated nonempty directory cannot be used as the destination.

`mirror=unchanged` says the directory already held this snapshot and a
verified archive, and nothing was fetched.

**At the next boot**, with no network:

```text
apkg channel-update ROOT DIRECTORY CHANNEL-ID NAME --policy FILE
                    [--target TARGET] [--takeover] [--repair] [--store DIR]
                    [--yes | --confirm DIGEST]
```

`apkg_client_channel_update` is one call for an unattended updater. In order:

1. An interrupted transaction is recovered. If recovery lacks objects, they
   are fetched again from the endpoint's archive of that build and recovery
   is finished.
2. An absent root is initialized with the pinned policy when a target is
   given.
3. A ready root whose policy is not byte for byte the pinned one refuses with
   `APKG_POLICY`.
4. The newest trusted candidate of `NAME` is installed or upgraded. A current
   package returns an UNCHANGED result.
5. With `--repair`, a current package is repaired when files are missing or
   damaged, its objects fetched again if the store lost them. Nothing is
   committed when nothing is wrong.

The endpoint is the mirror directory, or any channel directory or URL. The
library call takes an observer, a result, the takeover flag, the store and
explicit budgets; without a confirm callback it approves each plan as made.
A mirror carries one package: a dependency that only the original channel
offers is not in it, and the update refuses rather than install without it.

## Authoring

A TOML with `kind = "system"` builds a system package: the drawer's content
maps to the root itself, so `subdir`, `icon` and `launch` do not apply and no
provenance copy of the TOML is added to the payload. `config` names files
relative to the drawer. See [authoring](authoring.md#system-packages).

A system tree holds files for other processors by design: the i386 modules of
GRUB on an x86_64 system, firmware loaders, keymaps and other data in the
Amiga hunk format. The builder and the engine therefore ask no CPU of the
files of a system package; a bundle's executables must still match the
target.

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
Timestamps are not preserved. On a filesystem that does tell case apart, a
path renamed by case only is written under its new spelling and the file under
the old spelling is left in place, unowned.

## Limits

| Bound | Value |
|---|---|
| Descriptor, state record and plan record | each within `min(max_object_bytes, max_memory_bytes / 32)`: 2 MiB with default budgets, about 8,000 listed files; up to the 16 MiB object ceiling with `max_memory_bytes` raised |
| Tokens of one canonical object | 1,048,576, and no more than half of `max_memory_bytes` holds at 32 bytes each: `APKG_LIMIT` beyond |
| Channel snapshot (`channel.json`) | the context's object limit, 16 MiB by default; it embeds the full descriptor of every retained build, about 250 bytes and 13 tokens per listed file |
| Payload of one package | 2,147,483,647 bytes (PKG/1) |
| Path | 4,095 bytes, 32 components, printable ASCII |

Planning, execution and verification index the lists by folded path once and
look paths up in the index: no step is quadratic in the number of files. A
4,000-file package plans, installs, verifies and upgrades within the default
budgets.

Planning and the two live checks of execution each hash every listed file
that exists; the readiness check hashes every object to place, and placement
reads each of them once more.

An upgrade fetches and verifies the whole archive of the new build, not only
the objects that changed.

Host tests establish the behavior above, process death included. Power-loss
durability and the AROS handlers require evidence on the target.
