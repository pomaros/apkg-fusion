<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common journal: initial initialization/install executor

This is the protocol for the first operation subset, following the immutable
context/plan model. Runtime qualification remains distinct from this protocol.
No self-update or multi-volume operation is enabled by this first executor.
Those operations must use this journal's participant/decision model when added;
legacy self-update records remain migration evidence, never a second new engine.

The journal is `<physical-root>.apkg-transaction`, a sibling on the same volume.
This permits durable intent before creating an absent root. A nonblocking lock
on the existing parent serializes cooperating mutations (coarser than per-root,
intentionally). Parent alias resolution and device/inode identity are checked.
Readers never lock or recover. A journal's presence reports PENDING, including
after the database commit while cleanup is unfinished.

Journal contents are bounded canonical `plan`, `plan-sha`, `old-state` (install),
`new-state`, private `tree` staging, `tree-id`, random `archive-id` and durable `ready` / `decision`
markers. A decision marker is authoritative intent to roll forward; progress
callbacks and intermediate audit observations are not commit decisions. Recovery
checks real state/content/identity; it never trusts a guessed last-completed step.
Only relative validated package paths and the exact bound root can be mutated.

| Boundary | Normal effect | Interrupted execution/recovery |
|---|---|---|
| Before journal | Validate approval, root snapshot, policy, destination absence, volume and lock | STALE/UNCHANGED; no managed writes |
| Preparing journal | Write/flush plan and before/after bytes; build/flush staging, record tree identity | Incomplete/unreadable intent is retained for diagnosis; never guess an external path to delete |
| Ready | Flush intent and stage before any managed publication | Without decision: preserve original state, remove only verified owned staging/publication |
| Install apply | Rename staged tree to absent package directory without overwrite | Without decision, restore by removing only the exact recorded tree if all bytes/entries still match; foreign changes require manual recovery |
| Decision | Create/flush decision marker, flush journal | Every subsequent recovery rolls forward; cancellation is deferred |
| State publication | Atomically replace state after comparing old/new identity; initialize publishes staged metadata/root without overwriting | Accept already-published exact new state, otherwise finish from retained bytes; different state is unresolved |
| Completion | Flush affected directories, retain accurate result, atomically archive journal, flush parent | Cleanup interrupted: repeat verification/archive; never apply twice |

Before commit, cancellation or write failure attempts restoration. Its result
separates UNCHANGED, RESTORED and NEEDS_RECOVERY from its error/cancel code. After
decision an error remains NEEDS_RECOVERY; no rollback across the decision.
Result storage is allocated before mutation. Missing remote archives cannot
prevent recovery: approved descriptor, payload and both state records are local.
Unrecognized journal features/schema, changed state, edited published files,
foreign entries or missing retained content block recovery and preserve evidence.

Initialization uses a staged complete root when absent, or staged `.apkg` when
the root directory already exists. The recorded tree identity distinguishes a
published tree from an unrelated replacement. Initial trust grants are part of
the approved new state. Legacy metadata and a pre-existing journal are refusals.

Fault checkpoints: 1 preparation, 2 ready, 3 payload published, 4 decision,
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

## Self-update extension contract (not enabled by this executor)

The future operation is `self-update` with required feature `self-update-v1`.
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

SELF-UPDATE must implement/fault-test preparation on each participant, loss of
one volume, held executables/restart, coordinator and participant interruption,
interrupted recovery again, changed/foreign executable bytes, concurrent updates,
old journal import and explicit rollback. Until those gates pass, common clients
refuse that operation, and fusion PkgManager disables the inherited self-updater.
