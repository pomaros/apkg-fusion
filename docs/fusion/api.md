<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Public context API contract

Revision 1, a source API under development. The [header](../../include/apkg.h)
is now implemented for the [initial common engine profile](initial-engine.md).
The former contract path forwards to it. Unsupported operation families remain
explicit refusals; this is not a stable release ABI.
[Data model](data-model.md) owns semantics, [serialization](serialization.md)
owns framing, and [status](status.md) owns measured evidence.

## Context and object ownership

The prefix `apkg_` avoids collisions with both inherited `pkg_` APIs and their
identical `PKG_H` guards. Clients compile against one matching contract version.
No stable binary ABI is promised yet. Every options structure is zero-initialized
and carries exact contract version/struct size; mismatch returns UNSUPPORTED.
Text uses pointer/length views, UTF-8 without embedded NUL. OS path conversion
is explicit and lossless or fails. Binary values use byte views. No pointer or
native struct layout is serialized.

| Object | Ownership/lifetime |
|---|---|
| Input options, bytes, observer | Caller-owned for the synchronous call. Engine copies anything it retains. Observer/user/token remain alive until the call returns. |
| Context | Owned by caller after successful open; no operation may be running when closed. All calls on one context are serialized by caller. Independent contexts share no mutable request globals. |
| Snapshot/package/plan/result/error | Engine-owned allocation transferred to caller, released with matching free. They retain their own backing storage and remain readable after context close. |
| Root/row/effect/result views, text and record bytes | Borrowed until their owning object is freed. Further immutable getters do not invalidate them. Do not free or retain pointers beyond the owner. |
| Installed inspection | Caller-owned immutable copy, independent of snapshot/context lifetime. Its typed view and descriptor/signature/relation slices borrow inspection-owned bytes. Free with `apkg_inspection_free`. |
| Channel attestation | Borrowed supplied-order view followed by retained evidence from `apkg_channel_attestation_at`, valid until channel free. Entry counts distinguish supplied and total observations; discovery JSON preserves complete signed observations and accepted-memory origin/reference facts; missing history/freshness never becomes approval. |
| Attestation | Caller-owned signed claim and captured-policy facts, independent of inputs, snapshot and optional predecessor owner. Views borrow it until free. Linked observed history never asserts latest/current approval; see [attestations](attestations.md). |
| Source profile | Client-owned `apkg_source_config` with endpoint/identity and optional exact observation. Read/get/free do not assert trust; open revalidates against the current read-only root and returns independent snapshot/channel owners. Explicit save uses private atomic replacement; see [source settings](sources.md). |
| Signed observation | `apkg_channel_observation` borrows the exact object/signature bytes until channel free. Copy both for offline re-reading through `apkg_channel_read`; current trust, root target and replay floor must be revalidated. No transfer origin or install permission is conveyed. |
| Update summary | Caller-owned aggregate independent of snapshot/channel lifetime. Entry arrays and text borrow it until `apkg_update_summary_free`; native clients deep-copy the entire view before worker exit. |
| Approval | Caller-owned, bound to the originating context session, physical root and plan digest. Invalid after context close; free remains valid. Single use for execute, including failed/stale attempts. |
| Event | Borrowed only during callback. `apkg_event_clone` makes an independent deep copy including strings/evidence, freed by the receiving thread. |
| Cancellation token | Caller-owned, one-shot, thread-safe request; never reset/reused for another operation. Free only after worker has returned and no requesting thread can access it. |

All allocating outputs are set to NULL on entry. NULL optional error-output is
allowed; errors are allocated when possible, so NOMEM can have no error object.
Callers pass empty output slots; replacing an owned object without freeing it
is caller error. Every free accepts NULL. Input NULL required handles/pointers,
invalid lengths or enum values return INVALID, except misuse of void lifetime
functions, which is a caller precondition violation. Getters require a valid
owner; they allocate nothing. Error messages are presentation, never parsed
for control flow. Error code/field/subject are structured.

Immutable objects may be read on different threads while kept alive. Context
calls are synchronous, run on the calling thread and do not spawn workers.
PkgManager keeps its native DOS Process worker. Callback execution is on that
worker; UI updates happen only after the UI receives an owned event copy.
Callbacks may clone events or request cancellation, but must not reenter any
context operation, close objects in use or block waiting for the UI.

## Open, read and consistency

`apkg_open` resolves an explicitly supplied root (or its nearest existing ancestor for an
absent root), checks resource options and allocates a context. Neither READ nor
WRITE creates paths, locks, recovers, fetches, initializes an id or writes logs.
WRITE grants eligibility for explicit mutation; it does not start mutation.
Initialization is an explicit planned operation, not a side effect of
open or query. The initialization planner/executor now implements that operation in the initial profile.

`apkg_snapshot_read` returns ABSENT, LEGACY, READY, PENDING or INVALID facts.
An inaccessible root returns IO rather than ABSENT. Unsupported schema or
required features yield an inspectable INVALID root with a diagnostic/raw
record where bounded reading is possible; oversize/malformed framing fails
LIMIT/INVALID. No implicit old-engine recovery or schema conversion runs.
Unknown identity/generation/target is represented by fact state, not zero.
PENDING may expose the last committed snapshot labelled with pending state;
it cannot present incomplete on-disk effects as committed installed state.

Snapshots capture one committed generation and digest with its installed rows.
The backend uses a consistent immutable committed-record set or detects a
concurrent change and retries up to three times, then returns BUSY. Reading must
not take a write lock or create lock files. A snapshot is never refreshed in
place. If a root opened as physically absent is created by another writer,
reopen its context to bind the new root; the old handle refuses inspection
until then. Its own explicit initialization/recovery rebinds on success. Its row count and indexed getters are stable; out-of-range returns
NOT_FOUND. Root target, pending facts and rows share that observation. External
user edits remain separately sampled live observations, not an atomic filesystem
snapshot; planning/execution revalidate them.

The snapshot digest is meaningful only with a KNOWN identity and generation;
an unreadable pending database must expose unknown facts, not a zero digest
claimed as a committed snapshot.

Snapshot/package/plan results retain all authoritative bytes. UI views are
convenience projections; full records carry provenance, required features,
relations, ownership and independent review/runtime facts. The initial header
covers installed inspection, not the entire catalogue/news/settings API.
Later typed query extensions must preserve these lifetimes and snapshot rules.

## Budgets and refusal rules

Defaults per context: 16 MiB per serialized object, 64 MiB working memory;
caller may explicitly supply larger budgets subject to representable `size_t`
and backend capabilities. Resource options must be nonzero after defaults,
object budget <= memory budget and neither allocation may wrap on 32-bit hosts.
Caller-retained immutable objects consume their own memory; these are separate
from the working-memory budget and each remains object-budget bounded. Query
materialization charges all rows/backing bytes against working memory before
ownership transfer. A resource limit is LIMIT; allocator failure is NOMEM.

Framing limits are depth 32, 100,000 members/elements total, 4096 UTF-8 bytes per
path, 255 per package name/version, 1 MiB per other text field. These are explicit
initial implementation limits, not truncation rules. Larger legacy data remains
preserved as source evidence but cannot be presented as fully decoded or mutated
through a backend that cannot handle it. Future limit increases require tested
resource behavior, not silent format reinterpretation. Size/progress fields
remain uint64 even on 32-bit systems.

## Signed reads, plans and approval

`apkg_package_read` consumes exact descriptor and detached envelope bytes,
checks content/signature framing, declared target and current trust policy,
and retains verified package facts. Missing signatures/unknown keys are typed
SIGNATURE/TRUST failures, not prompt callbacks. Legacy unsigned records use
migration inspection, not this signed-read entry point. New-author trust must
be granted through an explicit policy operation, not `accept everything` flags.
No network or root writes occur during this call.

`apkg_plan_initialize` handles an ABSENT common root (an existing directory
with no common or legacy database may also be ABSENT). It requires an explicit
concrete target and canonical seed policy. The plan allocates a proposed random
root id in memory, binds the absent-state observation/ancestor/destination and
shows creation plus every initial trust grant. It creates nothing until approved
execution. Initial plan generation is 0; successful initialization publishes
generation 1. A competing initialization invalidates the plan under the
filesystem's first-root lock. Legacy metadata or foreign `.apkg` entries are
conflicts, never overwritten. Implement this path in PLAN-INSTALL/INSTALL with
first-root tests; CONTEXT only inspects it. Empty seed authorities are valid but
cannot authorize a signed install until an explicit trust-policy change.

`apkg_plan_install` accepts a common READY snapshot and exact verified package.
It checks physical root/snapshot binding, target, installed state and live
preconditions and produces canonical immutable plan bytes plus typed effects.
The first tranche plans one signed package without dependency changes, trust
changes, root initialization or system takeovers. Unsupported work returns
UNSUPPORTED or a non-executable plan with structured blockers, never a weaker
plan. PENDING returns RECOVERY_REQUIRED; missing/uninitialized root returns
NOT_FOUND; LEGACY needs migration. Dependencies already satisfied must still
bind exact providers and checks. Initial simpler implementations may refuse all
nonempty dependencies explicitly until supported.

Planning writes nothing to the managed root. Future downloads require explicit
cache/transport APIs and budgets; no hidden fetch is implied by this signature.
Every effect includes destination, before/after content/attribute references,
ownership/preservation policy and preconditions in its full record. The client
must show all material effects and blockers, including privilege/trust/system
changes in later operation families; a summary count alone is insufficient.

After caller confirmation, `apkg_plan_approve` requires a WRITE context and the exact displayed
plan digest and an executable plan from that session/root. It returns an opaque
single-use approval. It cannot detect whether a human really confirmed; the
caller is the authorization boundary. Serialized bytes, a copied digest or a
remote approval are not themselves an approval object. Session binding and
fresh precondition checks prevent accidental reuse, not a malicious application
with the same filesystem privileges.

`apkg_execute` requires WRITE and an approval matching the plan. Before any
managed write it locks physical root identity, rejects pending recovery and
checks committed generation/digest, policy, target and all live preconditions.
A stale plan returns STALE/UNCHANGED and consumes approval. It never re-resolves
or silently broadens approved effects. Execution refuses cross-root/volume
work until the participant protocol is implemented. Cache and root changes,
when eventually allowed, must be distinguished in result/plan records.

## Results, cancellation and recovery

Mutation allocates its result container before the first write. If allocation
fails, return NOMEM with no effects. Once any mutation begins, result must be
returned even when later allocation, recording or restoration fails; keep
bounded emergency status/transaction identity in preallocated storage.
`status` equals the function return code; `outcome` independently states disk
outcome. Rich diagnostics may be unavailable under allocation failure.

| Outcome | Meaning |
|---|---|
| UNCHANGED | No managed effect occurred, or recovery found nothing to do. |
| COMMITTED | Approved effects and required commit/durability steps completed. |
| RESTORED | An attempted operation failed/cancelled and restoration completed. |
| NEEDS_RECOVERY | Partial/uncertain state remains; transaction identity/evidence retained. |

OK execute requires COMMITTED. Failure/cancellation can be UNCHANGED, RESTORED
or NEEDS_RECOVERY; restoration is never inferred from an error code. A failure
after commit decision but before required completion yields NEEDS_RECOVERY,
not a misleading clean success. Explicit recovery may return OK/UNCHANGED,
OK/RESTORED or OK/COMMITTED according to what it resolved.

Events have operation id, monotonic sequence, phase, uint64 progress, unit,
known/unknown total and cancellable boundary. Progress is advisory; the returned
result is authoritative completion. No terminal callback is required to obtain
the result. A UI queue must be bounded. It may coalesce progress but must preserve owned diagnostics
and deliver the final result separately. Event-copy allocation failure requests
cancellation; it cannot assert cancellation already succeeded.

Cancellation is a request polled at safe boundaries. During commit/restoration
it can be deferred; late cancellation may finish COMMITTED with OK and
`cancellation_requested = 1`. Closing a window requests cancellation and waits
for the worker result before freeing context, observer or token. Repeated
requests are harmless. Cancellation must not abandon required recovery halfway
without reporting NEEDS_RECOVERY.

`apkg_recover` is explicit, WRITE-only, locks before reading mutable recovery
state and follows the common journal's recorded authorization. It is not an
arbitrary install or trust-grant API. Recovery must be repeatable after its own
interruption. Pending legacy operations refuse automatic translation until the
MIGRATE/SELF-UPDATE protocol is implemented. JOURNAL determines durable ordering
and guarantees; this API does not claim power-loss safety.

## Validation and implementation handoff

[Executable examples](../../tests/contracts/consumer.c) exercise inspection and
UI event ownership against a deliberately small
[test double](../../tests/contracts/fake.c). It has no filesystem, package
verification, planning or mutation engine. Plan/execute client flow is separately
compiled to check declarations. These checks establish client usage/lifetimes
in the scenarios simulated; they cannot establish runtime engine behavior.

The implementation profile and `make test-apkg` now provide real engine tests.
The original contract examples remain useful declaration checks.
Run `python3 tests/contracts/check.py`. It also checks serialization/signature
transcript vectors and strict rejection cases. The existing provisional clients remain regression baselines; the common CLI
and fusion PkgManager path use the new API. No contract fake is linked into
the common product. See the implementation profile for supported schemas and
conservative materialization budgets.

Design review checks: snapshot stability after context close, errors without
allocation, GUI callbacks after window cancellation, generation versus live
edits, approval reuse/cross-root transfer, post-commit cancellation, and limits
on 32-bit hosts. These are future engine test obligations as well as API rules.

The implemented operation, local dependency and client APIs are described in
[the operations profile](operations.md), including exact observed-tree plans,
current-trust offline rollback and installation reasons. Native runtime gates
remain independent from host API and cross-build evidence.

The explicit write-context archive importer and content-reference rules are
defined in [streaming payload transport](payload.md). Signature/metadata reads
remain side-effect free; import can retain verified objects before confirmation.

The [installed inspection profile](inspection.md) defines independent ownership,
captured-policy signature/author facts, root-target comparison, declared payload
counts, complete relations and shared CLI/PkgManager information fields.

[Candidate comparison](updates.md) adds captured root/channel refresh, exact
installed descriptor digests and shared metadata ordering without asserting
dependency-plan readiness or live payload health.
