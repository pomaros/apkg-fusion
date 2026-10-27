<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Fusion implementation plan

Build one package manager that preserves the strongest capabilities of both
projects and improves their shared guarantees. Choose the architecture by
behavior and evidence, including adopting arospkg's structure where it serves
the common product better. The repository used for integration does not decide
the design. The product engine is a new common implementation in `src/apkg/`
that follows arospkg's context model and links pkg's services; see the
[implementation decision](docs/fusion/architecture.md#implementation-skeleton).

The [architecture](docs/fusion/architecture.md) defines the contracts, the
[acceptance matrix](docs/fusion/acceptance.md) defines release obligations,
and [status](docs/fusion/status.md) records measured progress. This plan owns
the implementation order. A completed experiment is evidence for its scope;
it does not complete a later integration gate.

## Establish the integration baseline

Preserve pinned public inputs, licences and both regression suites. Establish
host build commands, separate experimental clients from released APKG, and
record every capability that must survive. Exercise a small complete path:
author TOML, application description, explicit target, signed publication,
installation and verification. Keep open publication independent of review.

Exit evidence: repeatable host tests, traceable source inputs, explicit gaps,
and independent review of newly introduced trust and target checks.

## Define common data and API contracts

Map real APKG registries and scripts before selecting the database schema.
Cover both existing distributions, custom roots, settings, edited files,
versions, packaging revisions, relations, installation reasons, launch data
and rollback history, including both existing self-update journals. Retain fixtures with permission and suitable provenance.

Define explicit contexts, typed queries/results/events, root identity,
CPU/OS/ABI/runtime, package/capability identity, version ordering and the
authoring schema. Separate read-only access, planning and mutation. Specify
event lifetimes, 64-bit progress, cancellation boundaries and recovery results
with the PkgManager worker model in mind.

Specify immutable plans that bind exact content, signers, selected providers,
destinations, current root state and authorized trust/system changes. Define
cache retention, offline recovery and initial trusted-key delivery. Preserve
empty directories, hidden payloads, icons and AROS attributes in the common
package representation.

Exit evidence: reviewed public API/schema, a shared version/target corpus,
migration mapping and executable examples for CLI and GUI consumers. No
database rewrite precedes this mapping.

## Build the common context engine and recovery

Use arospkg's explicit-context and journal concepts, integrating pkg's portable
filesystem, signing, ownership, system placement and publication services.
Move mutable operation state into contexts. Make read-only opening genuinely
side-effect free. Keep one mutation engine and one authoritative installed
database.

Implement plan execution with locked precondition checks, compatible installed
provider preference, explicit conflicts/replacements and preservation of edits.
Record recoverable intent before filesystem changes. Make recovery itself
repeatable. Establish filesystem capability and durability requirements before
claiming support for older DOS/handler combinations.

Exit evidence: stale-plan rejection before writes; install, upgrade, remove,
repair and rollback across dependency sets; first-root concurrency; failures
between mutation and recording; interrupted recovery; offline rollback. Distinguish
cancellation, process death, write failure and power loss in the results. Self-update uses this same journal and recovery
protocol, covering running-executable replacement and the old
`ENVARC:arospkg/self-update.json` history. It must not retain a parallel recovery
engine.

## Connect PkgManager and the native CLI

Connect PkgManager's browsing, recent additions, plans, cancellation, settings,
Run and folder opening to the common context API. Retain its native DOS Process
worker and copy event payloads across the UI boundary. Show signer/trust,
review, runtime evidence and system modifications distinctly.

Expose every publishing, system-management, diagnostic and automation workflow
through the native CLI, with shared operation semantics and structured results.
Implement standalone bundle launch, then managed adoption and movement. Check
the actual executable, filesystem containment and loader requirements.

Exit evidence: UI and CLI execute the same approved plan and produce equivalent
state; a manually unpacked bundle runs without a registry; cancellation and
close-during-work preserve a diagnosable outcome. A parser pass alone does not
accept launching.

## Complete migration and protect existing APKG users

Build the importer from the agreed mapping. Preserve original records, validate
before commit, support retry after interruption and report rollback-history
treatment. Keep the original installation usable on failure. Adapt the released
APKG command/script behavior through explicit compatibility entry points.

Exercise migration on both existing distributions before completing client
rollout. Retain old catalogue URLs through the upgrade path. The original pkg
side needs no legacy importer or command compatibility; adapt its consumers
directly to the common model.

Exit evidence: fixture and distribution runs cover edits, relations,
replacements, custom roots, interruption, retry and recovery, including clients
older than APKG 0.5 where their upgrade route applies.

## Complete authoring, channels and the portal

Run this work alongside engine/client integration after the common schemas are
defined. Extend the TOML bridge to every semantic field and payload shape.
Publish from directories, shares and web servers without a central account.
Preserve the portal's self-service accounts, resumable push, discovery,
provenance and administration.

Keep publication open and review optional. Bind review and runtime evidence to
exact digests and targets, preserve revocation/history and prevent authors from
self-granting qualifications. Add portable signed attestations, channel signing,
trust bootstrap and key rotation. Integrate the catalogue's review tools with
this model, using the same author description.

Exit evidence: an author publishes without review; maintainers qualify that
exact build independently; another build inherits no qualification; host
publication is consumed by CLI and PkgManager on AROS; local use works while
the portal is unavailable.

## Qualify platforms and adapt consumers

Test macOS, Linux and Windows host clients, AROS x86_64 ABIv11, i386 ABIv0,
mainline x86_64 ABIv1, native Pi aarch64 and hosted aarch64. Build evidence and
runtime evidence are separate. Experimental targets carry explicit coverage.

Adapt existing pkg consumers against the qualified interface, updating their
source pins, build recipes, commands, result parsing and API use in the same
change. Product-specific/private integrations stay in their own repositories.
Avoid migrating operational processes onto the provisional command shim.

Exit evidence: each consumer's own checks pass against pinned common builds;
both inherited suites and the common integration cases run on final sources.

## Extend and qualify distribution management

Develop fresh-volume installation and complete distribution updates with
distribution-defined package sets, configuration preservation, resident
components, deferred activation and boot fallback. Agree and implement the
Installer interface with its maintainers; start with file installation and a
configuration edit, including cancellation and failure.

Exit evidence: a fresh target volume boots; interrupted updates recover;
failed activation returns to a usable system. Rollback claims cover only
recorded effects. Application installation tests cannot establish these results.

## Release gate

The [ALIGN handoff](docs/fusion/maintainer-agreement.md) records feedback during
development. Both maintainers agree the concrete first-release scope and platform evidence.
Deferred capabilities retain their acceptance obligations. Publish a merged
release only after migration, trust, recovery and the included workflows are
qualified. The development repository and its passing host tests are not a
release verdict.

## Active goal: FUSION-ALPHA

Deliver an end-to-end, host-qualified fusion alpha using the common engine by
default. Move beyond synthetic inline package folders to the real authoring,
publication, discovery and client workflow. Preserve the broader acceptance
matrix; an alpha is not a joint release or AROS runtime qualification.

Work proceeds as one authorized sequence, with one agent,
focused tests and a recorded evaluation at each gate. Implementation evidence
and unresolved facts belong in [status](docs/fusion/status.md).

| Gate | Concrete result | Evidence required before advancing |
|---|---|---|
| ALPHA-PAYLOAD | Bounded archive/content transport for the common engine, replacing the inline-only practical limit. Preserve declared files, hidden/empty paths, icons, configuration and metadata meaning. | Author/package/installed inventories agree. Hostile archives, oversized input, tampering, collisions and interrupted retention/publication refuse safely. Unrepresented semantics stay explicit blockers. |
| ALPHA-AUTHOR | Existing TOML authoring drives common descriptors, bundles, archives and signatures without manual JSON construction. | Real authoring fixtures round-trip versions/revisions, targets, supported relations and payload metadata; existing unsupported families are refused rather than silently translated. Inputs remain unchanged. |
| ALPHA-CHANNEL | File and HTTP channels feed context-owned discovery, bounded fetching/cache retention, signature/trust validation and exact-content review/runtime facts. | Genuine signed publication is discoverable and installable. Changed channel/content, unknown keys, stale qualifications, network interruption and offline recovery are tested. Open publication never requires curated approval. |
| ALPHA-CLIENTS | Default `apkg` and the fusion PkgManager use the common engine for this workflow, including browsing and the implemented operations. Adapt current repository consumers; keep imported engines as explicit regression baselines. | CLI and actual worker adapter select and execute equivalent plans. Default product symbols/builds exclude the provisional global request backend. No silent fallback to another writer or truncated operation identity. |
| ALPHA-IMPORT | Legacy validation, import/commit and command-compatibility rehearsals preserve originals and provenance. | At least one fully specified pinned fixture migrates and remains operable; ambiguity refuses with diagnostics. Interrupted import/retry preserves old usability and retained records. Unknown distribution identities remain unknown; fixture passes do not certify either live distribution. |
| ALPHA-PORTAL | Portal consumes/publishes the common contracts with independent qualifications; migration tooling and a backup/restore/cutover-rollback rehearsal are reviewable. | Portal tests and an end-to-end publication/consumption scenario pass. Rehearsal preserves publisher/trust identities, object identity, URLs and review provenance; no live data or credentials enter the public repository. |

The [signed-channel profile](docs/fusion/channels.md) divides ALPHA-CHANNEL
into authenticated metadata, atomic file publication, context-owned file/HTTP
transport/freshness, and independent qualifications/client discovery. The
position of each gate is recorded in the
[status overview](docs/fusion/status.md#alpha-progress-at-a-glance).

Final acceptance is one automated clean-workspace scenario: author a bundle and
its dependency, package/sign/publish through a real channel, discover through
both client paths, preview/approve/install, preserve a personal configuration
through upgrade, repair, roll back offline and remove safely. Exercise refusal,
cancellation, failure and interrupted recovery around this path. Both inherited
suites, integration/memory checks and portal tests must pass in exact-source
Linux/macOS CI. Record remaining acceptance obligations without promoting a
host or fixture result into a platform/release claim.

Order of the remaining workstreams: finish the non-portal fusion workstreams,
then run an isolated M1 runtime rehearsal, then adapt the portal. The existing
M1 updater uses `libpkg` and is migrated by its own owner. Fusion tests never
replace its installed library or change its update/startup process. Candidate
binaries, test roots and recovery fixtures stay separate, and native results
are recorded by exact build. That rehearsal does not establish power-loss
durability or qualification of other distributions.

Other AROS execution and power-loss/distribution tests remain deferred. This goal
changes no OS repository and authorizes no deployment, live portal cutover,
maintainer contact or joint-release announcement. Actual production inventory,
distribution evidence and maintainer agreement remain separate release gates.
The portal deliverable is a tested, reviewable migration rehearsal; production
cutover follows after the fusion is qualified and real operational inputs exist.

## Immediate work sequence

BASE, MAP, CONTRACT and the first operation, channel, client and import
checkpoints have code; their evidence is in [status](docs/fusion/status.md).
The remaining non-portal work, in order:

1. Trust administration and the root source list have code and host tests;
   a signed key-endorsement format remains undefined.
2. Channel sources: an explicit revocation/freshness policy for online
   sources, and PkgManager controls for the root source list. Self-update
   copies on other volumes wait for the multi-participant protocol.
3. PkgManager: settings, news and the ARexx surface over the common client API.
4. ALPHA-IMPORT completion: relations, placements, settings and launch
   conversion, prior-version history and the command-compatibility adapter.
5. The clean-workspace alpha scenario has a host test; run it with both
   inherited suites and the portal tests in exact-source Linux/macOS CI.
6. The isolated M1 runtime rehearsal, then PORTAL-MIGRATE.

Host and native evidence stay distinct. An AROS type check is not runtime
qualification, and no step above claims power-loss durability.

## Bounded execution batches

Use this section to assign work without reopening the whole architecture.
Each batch ends with its artifact, targeted test result and a short handoff.
One batch is the default task boundary. When the operator explicitly authorizes
a sequence, evaluate each batch, resolve failures and continue through that
sequence; do not stop merely because the next batch needs implementation.
Complexity is relative engineering scope, not a token or time estimate.

| Batch | Dependency | Concrete deliverable and paths | Acceptance and stop boundary | Scope |
|---|---|---|---|---|
| BASE | None | Preserve the tested integration, source commits and precise `docs/fusion/status.md`. Publish the repository and plan. | Record the running suite's result, confirm the public revision, stop implementation. | Small |
| ALIGN | BASE | `docs/fusion/maintainer-agreement.md`: record feedback on skeleton, recovery, official trust and first-release scope. | Development proceeds while feedback is pending; revise decisions if evidence warrants it. Contact requires operator authorization; joint-release agreement remains a release gate. | Small coordination task |
| MAP | BASE | `docs/fusion/migration.md` and anonymized `tests/fixtures/migration/`; inspect imported `core.c`, `ops.c`, `swap.c`, `relations_io.c`, settings and release fixtures. | Identify the distribution/releases and DOS/handler versions as evidence permits; unconfirmed identities stay explicit and do not block contract work. Map roots, edits, revision, launch, reasons, relations, rollback and both self-update histories. Distinguish real-distribution fixtures from synthetic ones. No importer or schema implementation. | Medium |
| CONTRACT | MAP | `docs/fusion/data-model.md` and a public context API draft. Detail the selected arospkg skeleton and the module boundaries for pkg services. | Decide database identity, version/revision, target, payload fidelity and trust; define context/query/plan/result lifetimes. Review once with high reasoning. No broad source rewrite. | Large; split data model and API into separate tasks |
| CONTEXT | CONTRACT | Common context creation/closing and read-only root inspection; one module and its tests. | Two roots remain independent; absent-root reads create nothing; pending recovery is reported. No mutating operation and no GUI changes. | Medium |
| SIGNED-READ | CONTEXT | Connect existing signing, channel and manifest readers to the context API. | Read a signed package with explicit target; reject tampering, wrong key and wrong target. Preserve existing cryptographic implementations. No installation. | Medium |
| PLAN-INSTALL | SIGNED-READ | Engine-owned initialization/install plans, typed inspection and preconditions. | Plan identifies exact bytes, signer, root, target, writes and permissions. Changes invalidate it. Cover explicit first-root initialization with seed trust policy and one package without dependencies. No filesystem mutation. | Medium |
| JOURNAL | PLAN-INSTALL | Journal/recovery protocol specification and fault-injection cases. | Specify every durable boundary, root locking, commit decision, interrupted recovery, offline retained data and self-update recovery in the common journal. High-reasoning review before implementation. No claim that the inherited journal already satisfies this. | Medium |
| INSTALL | JOURNAL | One signed-package plan executor and journal recovery. | Install, write failure, cancellation and process termination produce stated outcomes; retry is idempotent; stale plan writes nothing. Keep OS power-loss qualification separate. No dependency solver, upgrades or UI redesign. | Large; split normal execution and injected recovery |
| GUI-INSTALL | INSTALL | One PkgManager worker path for preview/confirm/install, preserving DOS Process ownership. | UI shows and executes the same plan; progress/cancel/results match CLI and disk state. Retain the existing UI. No general rework of browsing or settings. | Medium |
| OPERATIONS | INSTALL | Separate tasks for upgrade, remove, repair and rollback, each with tests and configuration preservation. | Complete one operation per task. Edited/foreign files and retained rollback bytes are covered. Keep separate evaluation gates; an explicitly authorized sequence may span these tasks. | Four medium tasks |
| RELATIONS | OPERATIONS | Separate tasks for dependency closure, installed-provider preference, conflicts/replaces and recommends; then plan execution across packages. | Use shared fixtures from both engines; prove loader compatibility and interruption outcomes. Each relation family is its own stopping point. | Several medium tasks |
| BUNDLE-LAUNCH | CONTEXT | Native launch of an unregistered application folder. | Check containment, icons, executable target and loader requirements on AROS. No database membership or successful package installation is required. Parser acceptance alone is insufficient. | Medium |
| BUNDLE-MANAGE | INSTALL + BUNDLE-LAUNCH | Separate tasks for managed adoption and movement. | Ownership, update identity and recovery follow the common context model while standalone launching remains possible. | Two medium tasks |
| SELF-UPDATE | OPERATIONS + JOURNAL | Connect signed client updates to the common journal; retire both independent update-recovery implementations. | Test concurrent update, changed executable, interrupted replacement/recovery, rollback and old self-update-record import. | Medium |
| MIGRATE | MAP + OPERATIONS + RELATIONS | Importer, retry/commit protocol and legacy CLI compatibility adapter. | Original records retained; failed import preserves old usability; real fixtures from both distributions pass. Break into validation-only, import/commit, interruption and compatibility tasks. | Several medium tasks |
| AUTHORING | CONTRACT + RELATIONS | Extend `tools/apkg-publish` field by field; preserve revisions, requirements, relations, hidden files, empty directories and attributes. | Remove a refusal only with an end-to-end fidelity test. One semantic family per task; no silent field dropping. | Several small/medium tasks |
| QUALIFY | SIGNED-READ + AUTHORING | Connect catalogue/portal/client qualifications, signed attestations and trust bootstrap/rotation. | Open publication remains independent; exact-digest review and runtime evidence survive transport; authors cannot grant review. Split attestations, client presentation and key lifecycle. | Three medium tasks |
| PORTAL-MIGRATE | Merged client/channel contracts + QUALIFY | Migrate the existing portal at https://aros-pkg.azurewebsites.net/ to the merged product after fusion. | Inventory and back up channels, immutable objects, publisher accounts/authorization, configuration and statistics; rehearse restore and migration; verify signed host/AROS consumption, publishing and existing URLs; retain a tested cutover rollback. Preserve actual trust identities and review provenance. | Several bounded tasks after fusion |
| CONSUMERS | Stable operation APIs | Inventory-driven changes in each consuming repository. | Adapt one consumer, its pin/build/API/CLI parsing and its own tests in one task. Private integrations remain private. Do not migrate to the provisional shim. | Small/medium per consumer |
| PLATFORMS | Relevant functionality complete | One CPU/ABI/runtime or host qualification per task, with exact binary identity. | Build and runtime results stated separately; DOS/filesystem requirements checked. No unrelated hardware changes. | Medium per target |
| DISTRIBUTION | Recovery + system operations qualified | Separate tasks for package sets, fresh-volume installation, configuration edits, deferred activation and boot fallback. | Each task has a physical/VM acceptance scenario and recovery evidence; coordinate Installer interface independently. | Several large tasks |

GUI browsing/news/settings integration follows the context query API in
separate tasks. Portal and authoring work may proceed independently after the
common contract, but parallel agents are not the default execution policy.

## Budget-conscious working rules

- Default to one agent and one named batch. Use light reasoning for bounded
  implementation against an approved contract. Reserve higher reasoning for
  CONTRACT, JOURNAL and a focused review of irreversible design choices.
- Keep the coordinating agent at the operator's selected reasoning level.
  Use budget-appropriate models for bounded delegated work, honoring the
  operator's model preferences. Give each agent only its required context
  and files; avoid full-history forks and routine fan-out.
- Read this plan, status and the named files first. Reuse the recorded audit;
  do not rediscover both repositories for every task.
- Before editing, state the deliverable, file scope and targeted test command
  in a few lines. Split a batch if it requires unrelated subsystems.
- Run focused tests once after the change. Rerun only failed or affected tests
  while fixing it. Run full inherited suites at integration gates or when
  changes genuinely cross their scope, not after every small edit.
- Reuse a build when its sources and flags are unchanged. Do not start several
  overlapping builds of the same output directory.
- Avoid broad formatting, renaming, speculative wrappers and optional features
  outside the assigned batch. Record discovered work in status for another task.
- End with changed files, measured result, remaining limitation and next batch.
  Do not autonomously start the next batch or spawn agents.
- Maintain useful existing tests. A shorter run is achieved through scope and
  build reuse, never by hiding failures or weakening acceptance.

Suggested task prompt:

> Execute the next explicitly assigned operation family from PLAN.md using one
> agent. Reuse the recorded common-engine evaluation, preserve its tests and
> add the operation's refusal/recovery cases. Report implementation and native
> runtime evidence separately. Do not expand into other operation families.
