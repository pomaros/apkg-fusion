<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Migration mapping (MAP)

This inventory maps the pinned inputs in [provenance](provenance.md) to the
preservation obligations in [architecture](architecture.md). It is evidence for
CONTRACT and MIGRATE; implementation evidence belongs to [status](status.md),
with the bounded [host copy adapter](legacy-copy.md) specified separately.
No installed root or production portal was modified for this inventory.

## Evidence and distributions

The inspected arospkg revision is
`8e6ab1858b32bbf7a80f2f1c885a11af69aeb9f7`; pkg is
`6940c7ad621993b866604b17c8248dcb949e52da`, with subsequent integration changes
recorded in Git. Source observations below apply to those inputs, not every
historical release.

The imported [using guide](../../components/arospkg/docs/guide/using.md) and
[ARexx guide](../../components/arospkg/docs/arexx.md) name AROS One 1.3,
x86_64 ABIv11. This is upstream documentation evidence, not a fusion runtime
qualification. The second distribution, deployed APKG releases, actual
`dos.library` versions and filesystem handlers remain unknown. The operator
has authorized proceeding while these are unknown. Contract work can proceed;
release migration qualification must still exercise the actual environments.

No independently collected distribution database or user script was supplied.
The upstream [guide registry](../../components/arospkg/tests/fixtures/docs-walk-registry.json)
is a useful public record, but its presence does not certify a distribution
capture. The [fixture inventory](../../tests/fixtures/migration/inventory.json)
labels it separately from our synthetic examples. All synthetic paths and
URLs are fictional; no archive or executable is supplied or executable.

## Roots and persistent state

| Source | Observed layout/behavior | Required treatment |
|---|---|---|
| arospkg root | Default `SYS:Packages`; CLI and PkgManager accept `--root`. Each package drawer is `<root>/<id>`. | Retain explicit root selection and root identity. Resolve DOS assigns/volume identity on target; textual equality is insufficient. Never search and merge unrelated roots automatically. |
| arospkg database | `db/installed/<id>.json`, `db/index.json`, `db/transactions/`, `db/doctor/`, `db/previous/`; root `cache/` and `tmp/`. | Inventory and preserve original bytes and referenced files before interpreting. Report diagnostics and unresolved operations. Do not treat cache/tmp as disposable before recovery inspection. |
| arospkg locks | `db/lock.owner`; boot identity in `ENV:pkg/boot-id`. | Never import a lock as a live common-engine lock or delete it based on age alone. Establish quiescence and root ownership before migration. |
| pkg roots | Root metadata under `.pkg/`, installed records in `.pkg/db`, keys in `.pkg/keys`, automatic-install marks in `.pkg/auto`; placement can target shared system paths. | No legacy importer obligation on this side. CONTRACT must retain signed identity, trust, ownership and placement semantics; adapt actual consumers explicitly. Do not combine databases by filename. |

Primary readers/writers: arospkg [core.c](../../components/arospkg/src/libpkg/core.c),
[ops.c](../../components/arospkg/src/libpkg/ops.c),
[swap.c](../../components/arospkg/src/libpkg/swap.c), and pkg
[root services](../../src/pkg_lib_root.c),
[internal contracts](../../src/pkg_internal.h).

The pinned `pkg_open` created root/database/cache/tmp paths before checking
`PKG_OPEN_READ`. The integration corrects that imported read path and exercises
the side-effect-free common context separately; see [status](status.md). Migration
inventory must never open a mutation context as supposedly passive observation.

## Installed record mapping

The upstream [metadata guide](../../components/arospkg/docs/guide/metadata.md)
and `registry_text` in ops.c describe schema 1. The schema number alone does
not indicate which optional features a record contains.

| Stored fact | Source semantics | Common preservation/validation obligation |
|---|---|---|
| `name`, `dir` | Package id and drawer under the root; writer uses id for both. | Preserve raw values; validate containment, case collisions and identity before using paths. Never accept arbitrary record paths as write authority. |
| `version`, `revision` | Upstream version string plus packaging revision; absent revision reads as 0. | Keep original string and presence/default provenance. Preserve prerelease and rebuild ordering; do not coerce into a host version parser. |
| `arch`, `abi` | Installed build. Pre-0.3 records can omit ABI; old readers substitute their client's ABI. | Record omission and source environment separately. Do not silently convert the fusion host's ABI into historical evidence. Runtime is not established by this record. |
| `subdir`, `origin`, `archive_sha256`, `archive_size` | Archive extraction boundary, location and verified archive identity. | Retain extraction semantics and exact digest domain. An archive hash is not a common signed-manifest hash or a signer identity. |
| `installed` | Installation/upgrade timestamp. | Retain as source history; not a trust or runtime qualification. |
| `contents[]` | Delivered file path, SHA-256 and size; directories not inventoried by core's recursive inventory. | Preserve delivered baselines. Compare live bytes separately: matching, edited, missing and unrecorded. Never overwrite baselines with live edited hashes. Preserve hidden files, empty directories and attributes found on disk; incomplete inventories require uncertainty reporting. |
| `icon` | `installed`, `kept-existing` or `none`, plus hash/size/member. | Claim ownership only for recorded installed icons. Workbench-edited icons survive migration/update/removal. A kept-existing icon is user/foreign state. |
| `requires_system[]` | Requirement and installation-time probe verdict/version/explanation. | Preserve original observation, distinguish unknown/missing/satisfied. Recheck for execution when appropriate; do not turn old observations into current qualification. |
| `launch` and drawer `application.json` | Stored launch data can reconstruct a descriptor during rollback; the drawer descriptor drives execution. | Keep both originals and report disagreement. Validate relative paths, actual executable/loader and containment in BUNDLE-LAUNCH. Migration must not launch programs. |
| `post_install_notes` | Notes shown at installation, retained for offline history. | Preserve without executing text or trusting it as instructions. |
| `reason` | `explicit` or `dependency`; preserved across upgrades. Older records may omit it. | Preserve recorded reason; absence stays unrecorded until a documented compatibility policy decides. Never infer orphan-removal authority from missing data. |
| `relations_schema`, relation lists | Recorded dependency/conflict/replacement information; presence distinguishes known-empty from unknown. | Keep presence and raw declarations, including unsupported fields. Do not discard unknown semantics or equate unknown with empty. |
| `previous.registry`, `previous.archive` | References to retained prior record/archive, possibly unavailable. | Validate files, digests, extraction boundary and root containment. Preserve original references and report available/missing/unreadable history individually. |

[relations_io.c](../../components/arospkg/src/libpkg/relations_io.c) falls back
to the local catalogue for pre-0.6 relations. That lookup selects a build by
id/arch/ABI; it is not evidence that the metadata is the exact originally
installed version. A future importer must label recovered catalogue metadata
with its provenance and exact-build match (or uncertainty), rather than
inventing recorded relations. Recommendations are omitted from installed
relation fragments; capture the catalogue separately if available.
[relations.h](../../components/arospkg/src/libpkg/relations.h) also makes clear
that replacement does not provide the replaced id.

## Settings, clients and scripts

The inspected CLI and PkgManager use `SYS:Packages` by default and accept
`--root`; the CLI also accepts `--index` and `--index-url`. No general persisted
preferences file was identified in these entry points. This is an inventory
limit, not permission to discard settings. Capture actual launch commands,
icons/tooltypes, application configuration, per-user GUI preferences and custom
assigns from representative installations before MIGRATE. Preserve application
configuration and Workbench icon settings as live user state.

The CLI's `run <id>` forwards following words, including options, to the
application. Preserve quoting, arguments, exit codes, JSON output and ARexx
contracts through explicit compatibility scenarios. Public test script examples
exist in the imported [self-update scenarios](../../components/arospkg/tests/selfupdate/README.md)
and [ARexx documentation](../../components/arospkg/docs/arexx.md). These are
upstream test/documentation examples, not scripts collected from either
operational distribution. Existing catalogue URLs and older-client upgrade
routes must be covered; a new endpoint alone is insufficient.

## Transactions and rollback

arospkg transactions keep `000-plan.json` and markers under
`db/transactions/<transaction>/`. `005-commit` is the commit decision;
intermediate markers are audit information. Swap operations stage a tree,
rename the old tree aside, publish the staged tree, write icon/history/registry,
then commit. Recovery must inspect actual state as well as markers.

`db/previous/<id>.json` and `cache/previous/<id>.<ext>` survive transaction
cleanup. This is prior-revision retention, not an unlimited rollback history.
The rollback path clears the previous references rather than guaranteeing a
rollback of a rollback. Missing cached archives already make rollback
unavailable. Preserve that limitation explicitly; migration cannot manufacture
missing bytes from an origin URL.

Pending/unreadable plans, stale locks, doctor records and staged trees must
survive the inventory. Migration must refuse to declare such a root settled.
JOURNAL must specify how legacy recovery completes or is translated before
common mutations, without concurrent old/new writers. Closing and re-reading
a plan establishes present readability, not power-loss durability. DOS and
handler atomic-replacement/durability capabilities remain target measurements.

## Both self-update mechanisms

| Mechanism | Source state | Migration/recovery obligation |
|---|---|---|
| arospkg | `ENVARC:arospkg/self-update.json`, format 1: from/to release, managed root/target version, file paths and old/new hashes. `.new`/`.old` siblings hold replacement/backup bytes. | Preserve journal and all siblings; inspect actual hashes. A pending update is not a completed install. Link managed-home package recovery and external copies in one common recovery protocol. |
| arospkg installation associations | `ENVARC:arospkg/copies.json` is authoritative; ENV copy is fallback/courtesy. It links copied apkg executables to an explicitly chosen home. ENV lock/result records are boot-scoped. | Preserve chosen associations; do not discover ownership by scanning for names. Do not revive boot-specific locks/task pointers during migration. Distinguish stage handoff files from the durable journal. |
| pkg | `src/pkg_selfupdate.c`: POSIX staging beside executable (or temporary staging for dry run/admin path); verified replacement via rename. Windows preserves `previous.exe` in staging and can require manual restoration if replacement and restore fail. | No equivalent persistent JSON journal is present in this implementation. Inventory retained staging/backups and report uncertainty, rather than inventing old transaction records. New self-update still belongs to the common journal. |

Sources: arospkg [selfup.h](../../components/arospkg/src/libpkg/selfup.h),
[pure journal reader/writer](../../components/arospkg/src/libpkg/selfup.c),
[AROS I/O](../../components/arospkg/src/libpkg/selfup_io.c), and
[pkg self-update](../../src/pkg_selfupdate.c). The plan's phrase "both histories"
means both mechanisms' observable state; it does not imply two equivalent
persistent journals. Never execute restart arguments found in imported state.

## Fixture coverage and next work

The [fixture README](../../tests/fixtures/migration/README.md) explains the
synthetic records and observations. They cover edited and foreign files,
missing history, legacy omissions, independent icon ownership, incomplete
inventories and interrupted self-update. The [host copy rehearsal](legacy-copy.md)
consumes current/omission records and rejects unresolved operations. Passing
synthetic fixtures does not establish release migration qualification.

Root identity, unknown metadata, ordering, delivered versus live inventories and
target evidence follow the [data model](data-model.md) and [API](api.md). The
[bounded batches](../../PLAN.md#bounded-execution-batches) retain the broader
MIGRATE obligations beyond the first host profile.

Before MIGRATE qualification obtain, with publication permission or an
anonymization procedure: actual database and prior records; referenced archives
and tree inventories/attributes; launch/settings/scripts; pending operation and
self-update states; exact distribution/APKG/DOS/handler identities. Cover
legacy ABI omissions, pre-0.6 relations, old catalogue URLs and pre-0.5 client
upgrade routes. Missing samples do not block CONTRACT, but synthetic fixtures
cannot substitute for release migration qualification.

## Host copy adapter

The [legacy-copy rehearsal](legacy-copy.md) implements a bounded schema-1 profile
using the [common adoption-set API](adoption.md). It preserves originals and
unknown facts, retains source records/history bytes, and commits only the exact
validated common plan. Broader relation/placement/settings/launch conversion,
command compatibility and distribution qualification remain separate obligations.
