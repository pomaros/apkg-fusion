<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common operations, local resolution and clients

This extends the [initial engine](initial-engine.md).
[Streaming payload transport](payload.md) removes the inline-only file-size
constraint for signed archives and observed transaction trees. Native AROS runtime tests
are deferred at the operator's request while its source repositories are being
cleaned. This work changes only the fusion checkout; it launches no OS and
performs no portal deployment or legacy migration.

## Operation semantics

Every operation is a pure plan followed by same-session digest approval and the
common executor. Plans retain the complete observed affected trees, modes,
physical identities and exact resulting database. A live edit after preview
invalidates execution. Unsupported metadata, unknown required fields and
unorderable upgrades are refusals, never permission to discard information.

| Operation | Behavior |
|---|---|
| Remove | Unregister the package and remove pristine owned files. Keep modified and unknown files, with the directories they need, in place. A residual unmanaged folder prevents an implicit later takeover. Refuse if remaining packages would lose a required dependency. |
| Repair | Reverify retained signatures against current root trust; restore missing/changed ordinary files from signed bytes. Preserve edited configuration entries and unknown files. Repair affects only the named package. Before-images remain in transaction history. |
| Upgrade | Require a newer ordered version or packaging revision from the same authority, preserving the target. Perform a three-way merge of signed old, signed new and observed content. Preserve edited/unknown files; remove pristine obsolete files. Refuse foreign collisions, case collisions and unsafe type changes. Keep the installation reason. |
| Rollback | Require a committed local history matching the current package state, selected explicitly or by the bounded latest-transaction lookup below. Reverify retained package signatures under current trust. Restore the recorded before-image, including actual pre-repair bytes, while preserving subsequent personal edits. Retained trees must still match the archive. Increment generation rather than restoring an old generation number. No network fallback. |

### Latest package transaction

`apkg rollback-last ROOT NAME` prepares a rollback of the unique applicable
retained package transaction. The named package must participate in that
transaction; the lookup never skips a later unrelated package change to find
an older transaction mentioning the name. `NAME` equal to `-` explicitly
requests the root transaction without a package filter. The API uses an empty
name for that case. `apkg rollback ROOT HISTORY` retains explicit-path selection.

Rollback undoes the whole transaction, including dependency participants. Its
confirmation states that scope and lists every package change before requesting
the exact plan digest. PkgManager's installed Rollback shortcut and ARexx
`ROLLBACK name` use the same named lookup and common held-plan approval. The
generic operation also accepts `-`, including when the latest change removed
the package and no installed row remains.

Selection uses the captured root identity, package generation, installed records,
policy and target, never archive filename order or modification time. Source-list
and channel-observation changes do not replace package history; rollback retains
their current records. Changed trust does not get rolled back to make an old
package acceptable. Multiple applicable archives refuse with CONFLICT, missing
applicable history or an unrelated name with NOT_FOUND, and damaged/incomplete
history refuses without mutation. The normal rollback validator then checks
the selected archive, retained trees and current trust before producing a plan.

The lookup inspects at most 100,000 parent directory entries and 4,096 matching
history names, with context object/memory limits on record reads. Reaching a
bound returns LIMIT without selecting a partial result. A root without a
discoverable parent does not support this shortcut. This feature reads common
history only; retaining an old APKG journal during migration does not convert
it into common rollback history. The historical CLI adapter's per-package
rollback semantics remain a separate compatibility obligation.

A signed regular-file entry may include `"config":"preserve"`. Other values or
unknown fields are refused. Configuration status cannot be inferred from a
filename. New default bytes remain in the signed descriptor even when a local
configuration is preserved. Repair deliberately authorizes replacing changed
non-configuration files; its complete preview shows those effects and local
before-images remain available for rollback.

Version comparison ports arospkg's tested numeric/pre-release/rebuild algorithm,
with its authorship retained. Unknown spellings or different pre-release kinds
are not guessed into an ordering. Root metadata and unknown package fields
remain strict. `db` is reserved for the legacy database, not a package name.

The current tree profile handles regular files and directories with content and
POSIX modes. It refuses links, hard-linked files, special mode bits, foreign host
ownership, ACLs, unrepresented AROS protection/comments and other extended
attributes before mutation. macOS process `com.apple.provenance` is host-owned
bookkeeping excluded from the managed inventory. General timestamp/attribute
fidelity remains an AUTHORING/PLATFORMS obligation; this is not a replacement for
the full metadata model. Working/object budgets bound observed and retained
inline data; a large plan returns LIMIT rather than truncating its effects.

## One journal, multiple participants

Initialization and the first-install journal remain readable. The common journal
also accepts action `changes`, with a strict body containing `action`,
`anchor_dev`, `anchor_ino`, `before`, `changes`, `generation`, `kind`, `new_state`
and `root_path`. Each participant has `before`, `dev`, `ino`, `name`, `next`.
Tree values are null for absence or canonical `files`/`mode` inventories. Names
are validated and unique; no participant chooses an arbitrary host destination.

Under the same physical-parent lock, the executor retains old/new database and
approved intent, stages all next trees and records their identities, then makes
`ready` durable. Each existing tree is renamed into `old-N`, then the staged
`new-N` is published with no overwrite. The commit decision is written only
after every participant and the old database have been revalidated. Database
publication uses the existing atomic replacement service.

Before decision, recovery validates all participants before restoring any, moves
published next bytes into retained `restored-N` and puts original trees back.
After decision it finishes the new database and keeps all before-images.
Edited/foreign published content blocks automatic recovery with evidence intact.
Recovery may be interrupted after a participant and repeated. Completed journals
are atomically archived under the existing sibling history convention. No second
lock domain, independent client writer or separate update-recovery engine is
introduced. Self-update remains a later operation on this journal.

## Explicit local dependency resolution

A descriptor may include `relations`, with exactly four arrays: `conflicts`,
`depends`, `recommends`, `replaces`. Entries have exactly `authority`,
`min_revision`, `min_version`, `name`, `target`. Each array is sorted uniquely by
authority/name. A requirement matches an exact authority/name/target with an
ordered minimum version and packaging revision. Cross-target or indeterminate
matches do not satisfy it.

The C resolver accepts explicit requested packages and an explicit signed
candidate pool. It reverifies every candidate in the current context, prefers
suitable installed packages, and uses deterministic bounded backtracking when
candidate choices conflict. Cycles are valid when the selected set satisfies
all requirements. An explicitly requested signed replacement can remove the
named installed package; the complete removal is part of the approved plan.
Conflicts never imply replacement. Recommendations are optional (C API opt-in)
and cannot prevent a valid required closure. Installed reasons are retained;
new explicit, dependency and recommendation choices are recorded distinctly.
Removal/repair/upgrade/rollback validate the complete resulting required graph.

Limits: 128 installed packages, 128 requested packages and 128 pool entries for
resolution; 64 search depth and 4096 decisions. Reaching a search bound is LIMIT,
not evidence that no solution exists. Pool order is caller priority, not an
invented ordering for unknown versions. The local directory adapter sorts
candidates deterministically. Network catalogue/channel adapters remain later
work. Capability and shared-library provider identities/search-path resolution
are not implemented by this package dependency schema; their unsupported fields
are refused. A package dependency is not a loader compatibility certificate.

## Shared clients and bundles

```text
apkg-core list ROOT [MATCH]
apkg-core show ROOT [MATCH]
apkg-core folder ROOT NAME
apkg-core run ROOT NAME
apkg-core remove ROOT NAME [--confirm DIGEST]
apkg-core repair ROOT NAME [--confirm DIGEST]
apkg-core upgrade ROOT SIGNED-FOLDER [--confirm DIGEST]
apkg-core rollback ROOT RETAINED-HISTORY [--confirm DIGEST]
apkg-core resolve ROOT SIGNED-FOLDER [--pool SIGNED-FOLDERS] [--confirm DIGEST]
apkg-core bundle-check FOLDER TARGET
apkg-core run-bundle FOLDER TARGET
```

The public client helper owns verification, planning, complete effect formatting,
confirmation and execution for both CLI and PkgManager. No wildcard approval or
plan regeneration occurs after confirmation. The GUI operation selector accepts
a signed folder, package name or history folder and an optional local pool.
Installed browsing/search, details, Remove, Run and Open folder use common root
queries; operation completion refreshes the common list. Existing preferences
and native DOS Process lifecycle remain. The row adapter refuses identities its
legacy display columns cannot represent; it never uses truncated names in a
mutation. Full network catalogue/news/update discovery and legacy ARexx
compatibility remain separate integration/migration work, not passes inferred
from local installed browsing.

Standalone application folders require no registration. Shared bundle checking
uses the existing application schema parser, canonical containment, executable
CPU evidence, explicit target and required Workbench icon presence. Links and
escapes are refused. Host checks never execute the synthetic binaries.
The AROS backend probes declared library requirements before launch, refuses
unsupported requirement probes, starts Workbench objects through its library,
and uses LoadSeg/RunCommand with a bounded icon/default stack for Shell mode.
A Workbench success means an accepted launch request, not a confirmed process.
Host launch refuses UNSUPPORTED. Native runtime qualification, icon/tooltype and
resident-library scenarios remain deferred; managed adoption/movement remain
BUNDLE-MANAGE, not implied by launching or manually moving an unregistered folder.

## Evaluation gates

Targeted tests precede each dependent step: remove/repair preservation and crash
cases, then upgrade/version/conflict cases, then offline/history rollback cases,
then dependency closure/backtracking/multi-package interruption, then actual
CLI/native-worker adapter equivalence and standalone bundle checks. The normal
suite injects process death, I/O failures, cancellation, interrupted recovery and
allocation failures. Children terminate even on a harness error. ASan/UBSan runs
real library lifetime checks and CLI/worker operations. All existing regression
suites remain enabled in CI; [status](status.md) records exact-source results.
