<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common journal: initial initialization/install executor

This is the protocol for the first operation subset, following the immutable
context/plan model. Runtime qualification remains distinct from this protocol.
Multi-volume operations are not enabled. Self-update of a client installed in
one root is an ordinary package transaction on this journal; see
[the single-root profile](#single-root-self-update). Legacy self-update records
remain migration evidence, never a second engine.

The active journal is `<physical-root>.apkg-transaction`, a sibling on the same
volume. Preparation first uses a mode-0700 sibling
`<physical-root>.apkg-prepare.<operation-id>`. Only after intent, local staging,
identities and `ready` have been flushed is that directory renamed exclusively
to the active path and its parent flushed. No managed publication starts before
this step. The draft name has the same suffix length as retained history, so it
does not reduce the existing filename budget.
This permits durable intent before creating an absent root. A nonblocking lock
on the existing parent serializes cooperating mutations (coarser than per-root,
intentionally). Parent alias resolution and device/inode identity are checked.
Readers never lock or recover. The active journal's presence reports PENDING,
including after the database commit while cleanup is unfinished. Private drafts
are not active transactions: readers see the unchanged root during preparation,
and cooperating writers still serialize on the parent lock.

Journal contents are bounded canonical `plan`, `plan-sha`, `old-state` (install),
`new-state`, private `tree` staging, `tree-id`, random `archive-id` and durable `ready` / `decision`
markers. A decision marker is authoritative intent to roll forward; progress
callbacks and intermediate audit observations are not commit decisions. Recovery
checks real state/content/identity; it never trusts a guessed last-completed step.
Only relative validated package paths and the exact bound root can be mutated.

| Boundary | Normal effect | Interrupted execution/recovery |
|---|---|---|
| Before journal | Validate approval, root snapshot, policy, destination absence, volume and lock | STALE/UNCHANGED; no managed writes |
| Private preparation | Write/flush plan and before/after bytes; build/flush staging and record identities in the private draft | No active journal or managed changes; a killed process can leave a private orphan, without blocking explicit retry |
| Publish ready journal | Flush `ready`, exclusively rename the physically bound draft to the active path, flush parent | The active directory is complete and recoverable; an existing foreign coordinator is never overwritten |
| Ready | Recheck before any managed payload publication | Without decision: preserve original state, remove only verified owned staging/publication |
| Install apply | Rename staged tree to absent package directory without overwrite | Without decision, restore by removing only the exact recorded tree if all bytes/entries still match; foreign changes require manual recovery |
| Decision | Create/flush decision marker, flush journal | Every subsequent recovery rolls forward; cancellation is deferred |
| State publication | Atomically replace state after comparing old/new identity; initialize publishes staged metadata/root without overwriting | Accept already-published exact new state, otherwise finish from retained bytes; different state is unresolved |
| Completion | Flush affected directories, retain accurate result, atomically archive journal, flush parent | Cleanup interrupted: repeat verification/archive; never apply twice |

A normal preparation failure removes only its own physically bound draft and
flushes the parent. Cleanup failure reports unchanged managed state with the
private diagnostic path; it does not fabricate a pending transaction. Replaced
drafts and unrelated orphans are preserved. Process-death orphans are retained
privately; garbage collection is not implemented. A preexisting active directory
with incomplete/corrupt intent still blocks recovery and is never automatically
deleted on the assumption that it belongs to an unpublished draft.

Before commit, cancellation or write failure attempts restoration. Its result
separates UNCHANGED, RESTORED and NEEDS_RECOVERY from its error/cancel code. After
decision an error remains NEEDS_RECOVERY; no rollback across the decision.
Result storage is allocated before mutation. Missing remote archives cannot
prevent recovery: approved descriptor, payload and both state records are local.
Unrecognized journal features/schema, changed state, edited published files,
foreign entries or missing retained content block recovery and preserve evidence.

Initialization uses a staged complete root when absent, or staged `.apkg` when
the root directory already exists. The recorded tree identity distinguishes a
published tree from an unrelated replacement. Recovery follows the plan's
recorded anchor, not the current filesystem: a root that was absent at
planning and exists without the exact new state was made by someone else, and
recovery leaves it and the journal untouched. A linked `.apkg` or state file
blocks recovery as it blocks reading. An existence check that fails is an
error, never an absence. Initial trust grants are part of
the approved new state. Legacy metadata and a pre-existing journal are refusals.

Fault checkpoints: 1 private preparation, 1201–1204 draft creation/records/staging/
ready-before-publication, 1205 active publication before parent flush, 2 ready, 3 payload published, 4 decision,
5 state published, 6 recovery start, 7 recovery completion before cleanup.
The test build supports injected I/O failure or process termination at these
points. Production builds have no environment-driven fault injection. Tests
must cover recovery interrupted again, changed live files, offline recovery,
competing processes, stale approvals and late cancellation. Process tests prove
process-death behavior only. Power-loss/DOS-handler claims require independent
runtime evidence; fsync/flush and atomic-replace capabilities must succeed or
mutation reports failure. Cross-volume participants remain unsupported until
SELF-UPDATE supplies a reviewed activation/recovery protocol.

Design review: intent precedes publication; init can be journalled before root
creation; no-overwrite directory publication protects foreign directories;
commit is separate from state replacement; all rollback bytes are local; recovery
is itself restartable. Directory/handler durability and uncooperative filesystem
writers remain explicit platform boundaries, not claims inferred from host tests.

Completed journals are renamed without overwrite to
`<root>.apkg-history.<random-id>`. They retain the plan, before/after records and
any restored payload. They are not active transactions. No incremental deletion
of an authoritative journal is allowed: a death during recursive deletion could
otherwise leave an unparseable active transaction. Rollback moves an owned tree
into the retained journal instead of deleting its bytes. Garbage collection is
an explicit future retention-policy operation; this subset does not prune.

## Single-root self-update

`apkg_client_self_update` and `apkg self-update` update a client that is a file
of a package installed in the root. The running client's path must name a file
of that package's descriptor, and its bytes must equal the recorded hash; an
edited client is repaired first. The candidate is the newest trusted channel
entry of the same authority, name and target that carries the client at the
same path. Incomparable newer releases refuse. The update is then the shared
channel plan, confirmation, journal and recovery: a crash before or after the
decision recovers to one whole release, and rollback is the ordinary offline
rollback. A check-only call reports without planning. The running program is
not claimed updated: the result requires a restart. PkgManager's **Update
arospkg** runs the same call from its DOS worker, for its own file and the
signed source shown in its window; the plan appears in the usual confirmation
window, and other PkgManagers running from the same drawer refuse the update.

Two cases refuse rather than guess. An `ENVARC:arospkg/self-update.json` from an
arospkg self-update needs that arospkg to settle it. A recorded copy in
`ENVARC:arospkg/copies.json`, such as `C:apkg`, lives outside the root, so its
update needs the multi-participant protocol below. Afterwards, a client file
that the upgrade kept as an edit is reported as a conflict.

## Multi-root self-update contract (not enabled)

The operation is `self-update` with required feature `self-update-v1`.
Its approved plan binds the exact signed release, managed home/root and every
explicitly associated executable copy, their current hashes, replacement hashes,
local backup/stage locations, required restart/deferred activation and trust
policy. Associations come from recorded user choice; finding a matching filename
is not ownership. The coordinator uses this same journal schema and decision
rule, not a parallel self-update journal implementation.

For one volume, the coordinator retains all old/new bytes and writes intent
before touching any executable. For multiple roots/volumes, each participant
first durably records its identity, local retained bytes and coordinator
reference. Locks are acquired in a stable canonical physical-root order; failure
to acquire all releases them without activation. Every participant must expose
pending coordinator recovery to ordinary clients. Unknown required features
block mutation. An unreachable participant/coordinator is NEEDS_RECOVERY, not
permission for that root to start a conflicting operation.

No executable activation begins until every participant has prepared and the
coordinator decision is durable. Before decision, recovery discards preparation
and preserves existing executables. After decision, recovery only rolls forward
from verified local bytes; it records per-file actual hashes and replacement
outcomes and publishes final managed records only when the declared participant
set is settled. A program still held open by the OS may require restart; the
result states deferred activation and cannot claim the running program was
updated. A handler refusing atomic replacement cannot be bypassed by deleting
an executable. A subsequent rollback is a separately authorized common plan,
not an implicit reversal of a committed coordinator decision.

Migration first inventories `ENVARC:arospkg/self-update.json`, copies associations,
managed-home package transactions and `.old`/`.new` siblings together. It either
settles the old recovery model before common cutover or translates a fully
validated, quiescent participant set while retaining originals. pkg's POSIX
staging/Windows `previous.exe` has no equivalent durable JSON intent; absent
identity/hash evidence stays unknown and requires diagnosis rather than guessed
activation. Imported restart arguments and task pointers remain inert.

The multi-root extension must implement/fault-test preparation on each
participant, loss of one volume, held executables/restart, coordinator and
participant interruption, interrupted recovery again, concurrent updates and old
journal import. Until then, recorded copies refuse, and the inherited
arospkg self-updater is not used by the fusion build.

The implemented `changes` action extends this same journal to removal, repair,
upgrade, rollback and multi-package installation; see the
[participant protocol](operations.md#one-journal-multiple-participants).
Old initialization/install records remain recoverable.
