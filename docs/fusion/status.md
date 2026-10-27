<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Implementation status

This is a development integration workspace. It does not replace released APKG
installations. The checkout starts from pkg with an arospkg import. The
[architecture](architecture.md#implementation-skeleton) selects arospkg’s
context-based core as the skeleton. The initial common installation path is
implemented below; remaining service/operation integration is listed separately.

## Alpha progress at a glance

| Workstream | Current position | Work still required |
|---|---|---|
| Payload/common operations | Working host archive transport, fidelity, dependency operations and recovery checkpoints | Keep broader unsupported semantics and native/power-loss qualification explicit |
| Authoring | Real TOML/.ameta → signed package → atomic publication works | Complete remaining broader authoring/target semantics before their release claims |
| Channels/trust | File/HTTP channels, exact selection, signed review/runtime observations and accepted memory use the common journal; explicit journaled trust-policy change for rotation, reset and revocation; a root-owned ordered source list with install by name across sources | Remote revocation/freshness policy, a signed key-endorsement format |
| Clients/default product | Shared CLI/worker, source profiles, catalogue observations and typed installed rows; default CLI/library/tools use the common engine | Remaining desktop settings/news/ARexx integration and native/consumer/compatibility qualification |
| Self-update | Single-root self-update of a managed client through the shared channel plan and journal, from the CLI and the PkgManager worker; legacy journals and recorded copies refuse | Multi-root participants for copies such as `C:apkg`; native qualification |
| Legacy migration | Schema-1 host copy import with recorded relations and system requirements, immutable approval and common recovery on pinned synthetic fixtures | Placements, active settings/launch/history conversion, script compatibility and distribution qualification |
| Portal | Existing service and regression baseline retained | Common-contract adaptation and migration/backup/restore/rollback rehearsal; no live cutover |

These are implementation positions, not six completed gates. The final clean
workspace scenario and exact-source Linux/macOS CI must qualify the resulting
product. Native qualification is not established. The operator has now requested
an isolated M1 runtime rehearsal after the other fusion work and before portal
adaptation; deployment and live portal cutover remain outside this run.

## BASE qualification

The complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40221131858)
passed on source [439366b](https://github.com/pomaros/apkg-fusion/commit/439366bd67db69bce8bab220ade3d48980d6e5e9).
All configured jobs completed successfully:

| Job | Verified scope |
|---|---|
| `fusion (ubuntu-latest)` | Common integration, full inherited engine suite, full inherited client/authoring host suite, public-library symbol check and documentation links |
| `fusion (macos-latest)` | The same complete suite and checks on macOS |
| `portal` | Native verifier build and all 107 portal tests, with zero failures or skips |

The [workflow](../../.github/workflows/host.yml) keeps all these gates enabled.
The branch’s [CI runs](https://github.com/pomaros/apkg-fusion/actions/workflows/host.yml?query=branch%3Amain)
provide exact-commit verification for subsequent documentation updates.
Host qualification does not establish AROS runtime compatibility or power-loss
recovery, and Windows/static cross-build qualification remains separate work.

BASE fixed a first-use portal-search cache failure, bounded a self-update prompt
for GCC, and corrected host-dependent test assumptions: path buffer sizes,
reverse DNS during fixture startup, GNU/BSD command syntax and memory units,
copy semantics, locale ordering, archive member order, server readiness and
ELF fixture targeting. CI installs the native verifier and archive/image tools
required by the existing suites. Test assertions and memory limits were retained.

## MAP inventory completed

[Migration mapping](migration.md) records roots, schema-1 records and legacy
omissions, edited files, icons/settings/launch metadata, reasons, relations,
prior-revision retention, pending transactions and both self-update mechanisms.
The [fixture inventory](../../tests/fixtures/migration/README.md) distinguishes
new synthetic records from the upstream guide fixture. No independent
real-distribution capture is available and no importer was implemented or run.

Two concrete constraints feed the next batches: imported `pkg_open` creates
paths even in read mode (CONTEXT must correct this), and pkg self-update has
staging/backup state rather than a persistent journal equivalent to arospkg's.
Pre-0.6 catalogue relation fallback also needs provenance/exact-version handling.
Distribution/APKG/DOS/handler identity and real settings/scripts remain follow-up
items. They do not block contract work, per operator direction.

MAP stopped before schema/importer implementation. BASE CI evidence remains
unchanged; MAP changed documentation and inert fixtures only.
Validation: five JSON files parsed; inventory paths/source references and
synthetic edited-file/self-update hashes checked; documentation link check
covered 58 files with zero broken links. No runtime suite was rerun for these
documentation/fixture-only changes.

## CONTRACT data model

The [data-model decision](data-model.md) defines the logical common database,
root/generation/snapshot identities, authority and build identity, versions and
revisions, target constraints, complete payload inventory, ownership/reasons,
relations, trust/qualification provenance and immutable-plan/history inputs.
It assigns modules to the selected arospkg context skeleton and pkg services.
This was a design deliverable. The six-lot implementation below now supplies
an initial common writer; the importer remains unimplemented.

A focused review covered missing legacy facts, authority rotation/name clashes,
cloned roots, generation versus live-file edits, reserved database paths,
shared system ownership, version spelling ties, missing rollback bytes and
multi-root self-update. The contract requires explicit handling of each.
This was an in-session design review, not independent maintainer approval.

The [decision corpus](../../tests/fixtures/contracts/README.md) contains 15
version cases and 8 concrete-target cases. A temporary C99 host harness compiled
with `-Wall -Wextra -Werror` passed 54 assertions against the existing arospkg
comparison/upgrade functions and common target parser/matcher. Exact opaque
identity is a documented common-model distinction; its future implementation
was not tested. Documentation links: 59 files, zero broken links. No full
engine/portal/runtime suite was needed or rerun for documentation and inert
fixtures; BASE remains the previously recorded implementation evidence.

## CONTRACT API and framing design completed

The [API contract](api.md), [draft C99 header](api/apkg.h) and
[serialization/signature framing](serialization.md) define explicit read/write
contexts, consistent independent snapshots, owning objects and borrowed views,
64-bit progress, event-copy ownership, cancellation boundaries, immutable-plan
approval and authoritative recovery outcomes. Explicit initialization includes
seed trust grants and an absent-root precondition; open/read never initializes.
Object-family schemas remain implementation-batch gates before those operations
can run. This completed the initial CONTRACT design; implementation evidence follows below.

`make test-contract` runs an executable inspection/UI event example against a
labelled in-memory test double, compiles initialization/install client flows,
checks 54 inherited version/target assertions and checks golden canonical
bytes/signature transcripts plus 19 malformed-input/integer rejection cases.
The same checks pass with AddressSanitizer and UBSan. Documentation link check:
61 files, zero broken links. These contract examples alone establish no production API implementation,
actual signature verification, AROS worker execution or filesystem behavior. The fake is outside all production source/install lists.

Focused design review resolved explicit first-root initialization/trust,
unknown snapshot identity, post-commit cancellation, approval lifetime/reuse,
callback ownership and allocation failure after mutation. No independent
maintainer approval or full engine/platform CI result is claimed for this task.
The contract check is included in `test-fusion` for subsequent CI runs.

## Six-lot implementation and evaluation

The requested CONTEXT → SIGNED-READ → PLAN-INSTALL → JOURNAL → INSTALL →
GUI-INSTALL sequence is implemented for the bounded
[initial engine profile](initial-engine.md). It produces `build/libapkg.a`,
`build/apkg-core` and a fusion PkgManager worker path. The common product does
not link the provisional global request backend or test fault controls. This
is a first local signed-install implementation, not the full merged product.

| Lot | Evaluation before advancing; subsequent hardening retained |
|---|---|
| CONTEXT | Actual open/read API and owned snapshots pass absent-root, independent-root, close/lifetime, permissions, pending state, malformed input, aliases and root replacement tests. Read-only contexts need no entropy source. Unknown legacy state remains explicit. |
| SIGNED-READ | Existing SHA-256/Ed25519 verify real test signatures; tampering, wrong signer/trust, signature domain, target tuple, contradictory ELF CPU, unsafe paths and unknown required semantics are refused. No installation was used to accept this lot. |
| PLAN-INSTALL | Initialization includes seed trust and exact root/target; installation binds exact descriptor/signature, state generation/digest, physical root and every effect. Tests establish no writes during planning, foreign-destination refusal, session-bound approval and stale-state refusal. |
| JOURNAL | Protocol/fault boundaries reviewed before executor implementation. Intent precedes publication, decision is distinct, and init is journalled before root creation. Completed records are atomically archived; no incremental deletion of authoritative intent. The spec also owns future self-update coordination and both legacy update mechanisms. |
| INSTALL | Success, I/O failure, process death before/after decision, recovery interrupted again, unknown/changed live content, lock contention, cancellation/late cancellation, approval reuse and callback-time state changes are covered. Hidden files/empty directories survive. Retained history supplies offline recovery. |
| GUI-INSTALL | The actual signed worker bridge passes shared CLI/worker state equivalence, confirmation, decline, close/cancel, progress-message allocation failure and balanced native-message ownership tests. It retains the same plan/context on one DOS Process while awaiting UI confirmation. CLI and PkgManager cross-compile/link for hosted AROS aarch64; native GUI/runtime acceptance remains a platform gate. |

Measured local checks: 31 common-engine/client test groups with additional
fault/cancellation matrix cases and 293 allocation-failure positions; `make test-fusion`; the imported client host
suite; common/provisional symbol checks; ASan/UBSan lifetime and CLI/worker
scenarios. The inherited engine suite exposed a documentation fixture's fixed
port collision with another local service; its runner now substitutes an
OS-assigned port and retains the documented command/readiness assertions.
The old-DOS atomic-write test now additionally covers an SDK without the
replacement operation, preserving its negative control and destination checks.
The complete local `make test`, `make test-fusion`, imported host tests,
ASan/UBSan checks, public-symbol checks and documentation links pass.
The branch CI links above provide exact-commit Linux/macOS and portal results.

The public implementation [profile](initial-engine.md) freezes this tranche's
supported schemas and explicit refusals. It uses bounded inline-payload local
descriptors; existing author TOML/channel/container adapters, dependencies,
upgrades, shared system placement, general UI query integration and migration
remain later batches. It does not silently drop their semantics. Legacy
mutations/self-update are disabled in the fusion GUI profile rather than routed
through another writer. The independent non-fusion baseline remains available
for regression work.

Platform limits: AROS cross-builds are compilation/linking evidence, not a run on
either distribution. No OS was launched or altered by this task. Atomic state
replacement needs an exposing SDK plus dos.library >= 50.84, and file/handler
flush support must succeed. Missing filesystem identities refuse mutation.
Power-loss, distribution GUI/runtime and cross-volume activation remain
unqualified. Portal deployment and operational consumer adaptation were not
performed.

## Six-lot source qualification

Source [8e680be](https://github.com/pomaros/apkg-fusion/commit/8e680beef07469f6f2d7fcbd0d0afea5c94dfd23)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40398764206):

| Job | Result |
|---|---|
| Linux fusion | Common engine/client tests, 293 allocation-failure positions, ASan/UBSan, both inherited suites, symbols and documentation links pass. |
| macOS fusion | The same complete gate passes. |
| Portal | All 107 tests pass, zero failures or skips. |

A local hosted AROS aarch64 ABIv1 cross-build of this source also links both
programs and verifies `main`, `__startup_entry` and common-engine entry points.
These binary hashes identify local build evidence, not a release:

| Artifact | SHA-256 |
|---|---|
| `apkg-core` | `09a14073b49597bf43ac57cfbe0fc7f63c6d316f022c178e1ef49a7079c7e0fb` |
| `PkgManager` | `121404318e37b42a6acd36b7794f9631a67a3ab75faf6ce1a3d2909801079e28` |

The SDK used for this compilation does not expose `ReplaceObject`; the compiled
profile therefore refuses common mutation at capability preflight. Installing
on AROS requires a suitable SDK/DOS/handler and subsequent runtime qualification.
Neither native execution nor power-loss durability is inferred from this build.

## Next five stages: operation and client integration

The operator authorized repair, upgrade, rollback, dependency/conflict planning
and their CLI/PkgManager/bundle integration, with transactional removal as the
prerequisite. Native AROS execution is explicitly deferred while the operator
cleans its repositories. No OS source checkout was changed or OS launched.
The [operations profile](operations.md) defines the exact schemas and limits.

| Stage | Evaluation before dependent work |
|---|---|
| Prerequisite: remove | Pristine files are removed; edited/foreign files and necessary directories survive. Missing packages, links, stale live trees and interrupted removal have explicit results. |
| Repair | Missing/changed ordinary files are restored from reverified signed bytes; declared configurations and unknown files survive. A named repair leaves other installed packages alone. Cancellation, I/O failure and process death are covered. |
| Upgrade | Ordered version/revision advancement, three-way preservation, obsolete files, foreign/type/case collisions and interruption are tested. The arospkg version comparator retains its authorship and uncertain orderings refuse. |
| Rollback | Current-state-bound local history restores actual before-images offline, including pre-repair damage, while preserving later personal files. Corrupt/missing retained bytes and stale history refuse. |
| Package relations | Required closure, installed preference, cycles, backtracking, signed replacements, conflicts, optional recommendations, recorded reasons and multi-package interruption pass. Direct install cannot bypass relation checks, and removal cannot break the remaining required graph. |
| Clients/bundles | Actual CLI and DOS-worker bridge produce equivalent database and payload results for repair, upgrade, remove, resolve and rollback. Native installed browsing/search/detail and Run/Open use common queries. Host bundle checks cover registration independence, moving an unmanaged folder, containment, CPU/ABI target and icon presence. |

Local qualification: 63 common-engine/client test groups pass, including 693
allocation-injection positions across original and new paths. I/O boundaries,
process death, interrupted recovery, cancellation/late cancellation, callback-time
database changes, live edits and unrepresented metadata are exercised. The
shared API's error code and filesystem outcome remain separate and consistent.
The complete fusion integration gate and ASan/UBSan CLI/worker scenarios pass.
Exact-source CI results are recorded with the published source qualification.

A test harness defect initially let a forked allocation-test child continue the
suite after an assertion. Those test processes were stopped; new children now
terminate in a finally block even on a harness exception. The corrected tests
pass without weakening their assertions. Parser/descriptor allocation failures
also preserve NOMEM through cleanup instead of reporting malformed input.

Scope limits remain explicit: local signed package directories and package-name
relations are implemented; full network catalogues/news, capability/library
provider selection, authoring adapters, migration and ARexx compatibility remain
later integration work. PkgManager's existing display row adapter refuses an
identity it cannot represent. General metadata/time/attribute fidelity remains
unqualified; unrepresented metadata is refused before mutation. Native launcher
code and cross-builds do not establish runtime success, actual GUI behavior or
power-loss durability. Managed bundle adoption/movement remains BUNDLE-MANAGE.

## Five-stage source qualification

Source [a896f04](https://github.com/pomaros/apkg-fusion/commit/a896f045f8177a25d6ebd4f417a6f1d1243bb022)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40404698438):

| Job | Result |
|---|---|
| Linux fusion | All 63 common test groups, 693 allocation-injection positions, ASan/UBSan lifetime and five CLI/worker groups, full fusion checks, both inherited suites, symbols and documentation links pass. |
| macOS fusion | The same complete gate passes. |
| Portal | All 107 tests pass, zero failures or skips. |

The first Linux run exposed missing POSIX declarations and GCC enum signedness
warnings, corrected without weakening compiler flags or assertions. A subsequent
runner spent several minutes downloading system test dependencies; dependency
acquisition now has explicit network retry/time limits and a five-minute step
limit. The successful run retains every required test dependency and suite.

Compilation/linking of the extended CLI and PkgManager was also checked against
an existing hosted AROS SDK, writing only fusion build artifacts. Native
execution, actual UI/launch behavior and power-loss qualification remain deferred
per operator direction. No operating-system source repository was modified.
The SDK atomic-replacement restriction documented above still applies.

## FUSION-ALPHA progress: streaming transport

ALPHA-PAYLOAD is in progress; this checkpoint implements real archive transport
and removes payload-byte duplication from observed tree plans/history. It does
not complete the alpha goal, the metadata-fidelity gate or later authoring,
channel, default-client, migration and portal gates in [PLAN](../../PLAN.md).
See the [transport profile](payload.md) for format, limits and retention rules.

Actual PKG/1 archives carry verified signed metadata plus SHA-256-addressed
objects. The common library, CLI and PkgManager worker import/install them.
Large observed files are streamed through hashing, retention, staging and
recovery. Old inline journal inventories remain recoverable. Partial cache
imports never create a managed commit or silently initialize a root.

Local evidence includes an archive containing a file above 32 MiB installed
with database/plan records below 10 KiB and individual instrumented engine
allocations below 1 MiB. This measures the tested allocation boundary, not an
unmeasured peak-RSS or platform-durability claim. Large-file upgrade, repair,
offline rollback and removal preserve personal configuration; history plans
stay bounded independently of the file sizes.

The test additions exercise hidden/empty paths, zero/shared blobs, signed CPU
mismatch, missing/extra/duplicate/traversal/truncated members, cold and corrupt
cache entries, corruption after approval, cancellable progress, import I/O and
process termination/retry, allocation failures and normalized old-journal
recovery without a cache. Existing common/inherited assertions remain enabled.
Host/native evidence remains distinct; no OS source was changed and no AROS
execution or portal deployment was performed.

Remaining ALPHA-PAYLOAD obligations include full declared metadata fidelity and
its round-trip inventory checks. Cache GC/reclamation and additional archive
formats are unimplemented and cannot be inferred from these tests. The private
object store includes before-images and must never be bulk-published as a channel.
Exact-source verification is recorded with the published implementation revision.

## Streaming transport source qualification

Source [03f9403](https://github.com/pomaros/apkg-fusion/commit/03f94033e70c6973ff50f6220cbbd0d037aadf3f)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40410182172):

| Job | Result |
|---|---|
| Linux fusion | All 76 common test groups, including 793 allocation-injection positions, pass. ASan/UBSan lifetime and six CLI/worker groups, full fusion checks, both inherited suites, symbols and documentation links pass. |
| macOS fusion | The same complete gate passes. |
| Portal | All 107 tests pass, zero failures or skips. |

This qualifies the streaming transport checkpoint only. ALPHA-PAYLOAD remains
open for full declared metadata fidelity and its author/package/installed
inventory round trips. The following checkpoint addresses the row-allocation
and path-text audit items; all later alpha gates remain open.

## FUSION-ALPHA progress: host metadata and inventory scale

The [host semantic metadata profile](payload.md#host-semantic-metadata-profile)
adds signed per-entry Amiga protection, comments, owner facts and ordered opaque
extension records. Host installation preserves these facts without assigning
Amiga principals as host users. Ordinary file modes and explicit directory modes
are preserved subject to read/traverse requirements. CLI and actual worker
previews use the same engine result and expose these facts.

Metadata participates in observed before-images, approval preconditions,
staging checks, history and offline rollback. Three-way merging preserves
personal metadata independently of updated file bytes; protection merges per
bit. Pristine removal removes the tree, while metadata edits retain the
otherwise pristine file. Unknown host metadata remains a refusal.

Path rows now own exact-size strings instead of fixed 4 KiB arrays. Directory
name storage grows geometrically and carries a cumulative metadata budget. The
1,200-file fixture installs, repairs and removes while preserving an added
personal file. Invalid UTF-8/control/NUL import paths refuse before I/O.

Local macOS evaluation:

- `make test-fusion` passes: 87 common methods, contract checks, inherited target
  (118), application (315), first-install (27), read-only context adapter,
  CLI (110) and publication (78) checks.
- The new metadata cases include archive round trips, nondefault directory/file
  modes, UTF-8 comments, full uint64 protection, uint32 owner boundaries,
  opaque extensions, personal edits, stale approval and four process-death
  boundaries with repeat recovery and offline rollback.
- 400 metadata-planner and 350 metadata-executor allocation-injection positions
  preserve data or produce explicit recoverable outcomes, in addition to the
  existing injection suites. Executor injection also includes a nonempty
  read-only directory. Private abandoned staging can be made writable for
  disposal; live directories and retained history keep their declared modes.
- ASan/UBSan lifetime and all seven client groups, including actual CLI/worker
  metadata equivalence, pass with both sanitized executables.
- Documentation links: 65 files, zero broken links.

This is a partial ALPHA-PAYLOAD checkpoint, not gate completion. Tests still
construct descriptors directly: source `.ameta`/TOML authoring, timestamp
semantics and a complete author/package/installed inventory round trip remain
required. Native explicit-metadata mutation refuses before staging pending
adapter/runtime qualification. No AROS execution or OS changes occurred; no
portal code, live data or deployment was changed. Exact-source Linux/macOS CI
must independently qualify this published revision. The preceding documentation
revision's Linux job timed out installing recommended font packages from its
Ubuntu mirror before compilation; macOS and portal passed. The dependency step
now installs required packages without optional recommendations, retaining all
regression commands and its existing timeout.

## Host metadata source qualification

Source [b40bcb4](https://github.com/pomaros/apkg-fusion/commit/b40bcb4833e866d402cb6b63f2a473c54b927c73)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40413222524).
Linux and macOS pass common integration, memory checks, both inherited suites,
public symbols and documentation links. The portal job also passes. This
qualifies the preceding host-metadata checkpoint; it does not close the alpha
or the remaining metadata/authoring obligations.

## FUSION-ALPHA progress: real source authoring

The [common authoring checkpoint](authoring.md) adds `tools/apkg-build` and
context-free source/key/signing commands to the common executable. Actual
apkg-pack TOML and `.ameta` now produce a signed PKG/1 artifact consumed by the
common CLI and PkgManager worker. The provisional global backend is absent from
this path. The existing publishing bridge/default client remain separate work.

Source/package/installed fixtures preserve hidden files, empty drawers, icon
bytes, ordinary modes, comments, full protection words, Amiga owner facts and
opaque extensions. Explicit and omitted defaults share one normalization.
Original TOML is retained byte for byte; versions, revisions, launch generation,
configuration and implemented relation families become common engine inputs.
The source remains untouched, including read-only directory modes. Existing pkg
keys retain the same public identity and produce the engine's signature framing.

The bundle inspector now streams executable inspection instead of loading the
whole program with a 16 MiB ceiling. A synthetic application above 32 MiB is
built and installed from its actual TOML; its executable is never run.

Local evaluation includes the complete fusion suite (101 common methods),
source-wrapper allocation
injection at 200 positions, ASan/UBSan lifetime and 21 client/source/authoring
methods, and 66 documentation files with zero broken links. New failure cases
cover malformed/stale/conflicting metadata, no-overwrite key/artifact creation,
damaged keys, wrong CPU, unsupported declarations, aliased output inside the
source, and artifact publication I/O failure/process death followed by retry.
Interrupted authoring may retain an unpublished private staging file; retry
does not confuse it with a channel artifact.

ALPHA-PAYLOAD and ALPHA-AUTHOR remain open for the stated broader fidelity and
consumer obligations. Parent `.ameta` selection for drawer/icon metadata and
nondefault source-root metadata refuse explicitly. Timestamp semantics are
still unqualified. File/HTTP channels, independent qualifications, default-client
replacement, legacy import and portal migration rehearsal remain outstanding.
No native execution, OS changes, live portal data or deployment occurred.
Exact-source CI for this new checkpoint is required separately from b40bcb4.

## FUSION-ALPHA progress: selected parent and snapshot metadata

The [authoring profile](authoring.md#source-and-metadata) now supports the
source drawer's nondefault semantic metadata and the declared sibling icon's
entry from a parent `.ameta`. The new read-only `source-entry PATH` selects
exactly one file/drawer. It checks the parent control's syntax without reading
unselected sibling file content or publishing their records. Unselected names
outside the payload may be absent; malformed controls and conflicting selected
live/control facts still refuse.

Source-root metadata is observed separately, then becomes an explicit authored
directory entry in the signed payload. It does not widen the mutation contract
for the engine-managed outer package folder. The builder rechecks selected
metadata and icon facts after copying. It also compares exact control-file
SHA-256/size observations and re-observes the source snapshot. Equivalent-looking
control edits still refuse when their bytes changed during copy.

Recognized semantic xattrs can be lost by a host's copy primitive. Validated
source facts are retained independently in the signed descriptor; file bytes,
paths, modes and control bytes must still agree. Newly introduced or conflicting
copied metadata refuses. Tests explicitly discard copied carriers and verify
installed directory/file facts, leaving source carriers intact.

Local evaluation passes `make test-fusion` with 108 common methods, including
140 additional selected-entry allocation-injection positions, plus the existing
contract, target, application, first-install, context, CLI and publishing checks.
ASan/UBSan lifetime and all 28 client/source/authoring methods pass. Focused
CLI/worker scenarios install drawer/icon metadata, preserve a personal drawer
comment through an authored update and repair, and retain configuration.
An unreadable private sibling and stale unselected parent entry never enter the
archive. Documentation links cover 66 files with zero broken links.

The parent-control and source-root refusals recorded at the preceding checkpoint
are resolved for this profile. Timestamps, wider metadata/platform fidelity and
the subsequent channel, default-client, legacy-import and portal gates remain
open. No AROS execution, OS changes, live portal changes or release claims were
made. This checkpoint requires its own exact-source Linux/macOS CI result.

## Parent/snapshot metadata source qualification

Source [9e183cb](https://github.com/pomaros/apkg-fusion/commit/9e183cbbb82b9c6d9ef74719b93ee7d7d91c2b60)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40415726785).
Linux, macOS and portal jobs pass, including common integration, memory checks,
both inherited suites, public symbols and documentation links. This qualifies
the parent/snapshot metadata checkpoint, without promoting it to native runtime
or complete alpha qualification.

## FUSION-ALPHA progress: authenticated channel metadata

The [signed-channel metadata profile](channels.md) adds public immutable channel
views and selected-descriptor decoding through the common context API. Discovery
checks channel identity, role-authorized signature, sequence floor, every
embedded descriptor/signature and conflicting build identities. Foreign targets
and locally unauthorized authors remain visible as explicit blocked facts.
Selecting a package rechecks the bound root, target and exact policy; channel
trust never substitutes for package-author authorization.

Policy now optionally includes independent channel-key grants. Initialization
shows and persists both role arrays under the approved plan. Existing policies
grant no implicit channel rights. Tests include shared identities across roles,
domain confusion, unauthorized keys, forged qualifications and stale policy or
sequence inputs. Cryptographic verification is factored into a shared pure
verifier while preserving the existing descriptor transcript and typed failures.

The common CLI can sign a channel object and inspect a supplied signed snapshot
folder. A fixture uses an actual TOML-built PKG descriptor and native channel
signing. This is metadata inspection only: archive availability and online
freshness remain explicitly unchecked/unknown. It performs no fetching, cache
creation, root initialization, trust changes or recovery.

Local evaluation passes `make test-fusion` with 118 common methods, including
240 additional channel-read allocation-injection positions, plus existing
contract and inherited integration checks. The sanitizer harness passes 38
client/source/authoring/channel methods. A dedicated native C runner checks
channel/package lifetimes after freeing input buffers, context and channel in
sequence under ASan/UBSan; Python allocation hooks continue to exercise the
ordinary test library. Native CLI refusal tests cover signature, role and schema
cleanup paths. Sanitizer errors halt execution. Documentation links cover 67
files with zero broken links.

ALPHA-CHANNEL is not complete. Atomic file publication, file/HTTP transport,
archive/descriptor binding, durable sequence high-water tracking, independent
qualifications and shared client discovery remain required. The profile records
these next batches explicitly. All other open alpha/release obligations remain;
no native execution, OS changes or live portal operations occurred. This source
requires its own exact-source Linux/macOS CI qualification.

## Authenticated metadata source qualification

Source [643e059](https://github.com/pomaros/apkg-fusion/commit/643e0596ff38eaee4d3308e20193c0865ae2d207)
passes the complete [Host integration run](https://github.com/pomaros/apkg-fusion/actions/runs/40417208331).
Both host jobs and portal pass, including inherited suites, common memory checks,
public symbols and documentation links. This qualifies the metadata checkpoint
only; file publication and HTTP integration require their own evidence.

## FUSION-ALPHA progress: atomic local publication

The [file-channel profile](channels.md#atomic-local-publication) now has an
actual producer, `tools/apkg-channel`, and context-owned read-only opening via
`apkg_channel_open_file` / `apkg-core channel-open`. A new explicit
`apkg-publish --common` mode connects real TOML/.ameta authoring, archive signing,
full native archive verification and publication without the legacy backend.
Author/channel identities and keys remain explicit; both secrets stay outside
the disjoint source/public-channel trees.

The producer verifies the entire selected archive through a discarded private
common root, retains SHA-addressed archive/snapshot files exclusively, flushes
files/directories, then atomically replaces one `current` reference. Stable OS
locking serializes cooperating publishers; process death releases the lock.
Retry checks existing immutable bytes instead of overwriting them, can finish
an unreferenced partial snapshot, and does not advance an identical publication.
It refuses corrupt retention, conflicting builds, changed references and silent
channel-id/key replacement. A post-replace flush failure reports the updated
reference with uncertain durability; retry verifies/finishes rather than
blindly restoring an older head.

Opening a file channel verifies its reference, snapshot digest, signatures,
identity, root roles and caller sequence floor, then rechecks the reference.
Selected descriptor decoding rejects a changed head. Views expose archive
identity/size without pretending the archive was transferred. Channel-aware
transfer and approved-plan preconditions remain subsequent work.

Local evaluation passes `make test-fusion` with 131 common methods and the
existing contract/target/application/context/first-install/CLI/publish checks.
The sanitizer harness passes 51 methods, with native channel/package lifetime
checks for both raw metadata and file endpoints. New file-open allocation tests
cover 280 injected positions without root/channel changes. Scenarios cover two
simultaneous publishers, four process-death publication boundaries, pre-commit
I/O and post-commit flush errors, stale-head refusal and retry. A real archive
above 32 MiB is published, discovered and explicitly installed with small
catalogue/database metadata; no whole-archive read is required by production
transport. FIFO inputs/current references refuse without blocking. Documentation
links cover 67 files with zero broken links.

This qualifies host process/IO behavior locally, not native DOS, Windows,
network-filesystem or power-loss durability. The inherited publishing bridge
still passes and remains the default until the consumer switch. ALPHA-CHANNEL
still requires HTTP/cancellable context transfer, archive/selected-descriptor
binding, durable high-water tracking, plan preconditions, independent
qualifications and shared client selection/fetching. All other alpha/release
obligations remain open; no live portal or OS operation occurred. Exact-source
Linux/macOS CI must separately qualify this implementation.

## FUSION-ALPHA progress: selected host transport

The preceding atomic-file checkpoint is now qualified by the full
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40419613570)
on source [27be6a8](https://github.com/pomaros/apkg-fusion/commit/27be6a8657a575dab2f60bc15003cbb25ef27dc1).

The next checkpoint adds [host HTTP discovery and selected transfer](channels.md#host-http-discovery-and-selected-archive-transfer)
to the context API and explicit common CLI. Discovery remains read-only. Fetch
prepares private archives and payload objects without committing installed state;
whole-file integrity and exact selected descriptor/signature binding are checked
before retention. Cache hits are reverified, source/policy are rechecked, and
progress/cancellation belong to the operation rather than legacy global hooks.

New HTTP/file tests cover real publish/discover/fetch/install, read-only refusal,
redirects and byte limits, malformed endpoints, missing/truncated/tampered bytes,
cache reuse and corrupt-cache refusal, callback cancellation during an archive
above 8 MiB, pre-request cancellation, three injected flush/publication failures
with retry, and a signed reference to a different valid archive. These establish
host process behavior; they do not qualify TLS interoperability, power loss or
native network adapters. Local `make test-fusion` passes all 139 common
methods plus the existing integration checks. The sanitizer harness passes
59 methods, including actual HTTP CLI discovery/fetch through instrumented
native binaries; direct ctypes API cases use the ordinary test library.
Documentation links pass for
67 files with zero broken links. Exact-source Linux/macOS CI must separately
qualify this checkpoint.

At that transport checkpoint, approved-plan origin preconditions remained open;
the following checkpoint implements them. Durable replay high-water state,
independent qualifications, native adapters and shared client
selection still require implementation. No default backend switch, portal
migration, deployment or AROS execution occurs in this checkpoint.

## FUSION-ALPHA progress: source-bound approved plans

File/HTTP selections now retain owned channel origins. Common install, upgrade
and dependency resolution include selected origins in canonical approval bytes,
deduplicate identical sources and exclude unused candidates. Fetch preserves
provenance through full archive verification. A plan therefore continues to
own its source facts after channel/package objects are freed.

The [source-bound plan contract](channels.md#source-bound-approved-plans) checks
root identity/policy/target, current reference, immutable snapshot bytes and
channel signature/role before mutation, after staging and before decision.
A changed source after live application restores the original local state.
Network errors cannot silently downgrade to unchecked cached installation.
Origins stay in plan/history records; installed-state records remain independent
of mutable endpoints. Decided recovery, retained rollback and repair use local
verified content without reopening a channel.

New scenarios cover both initial-install and multi-change transaction paths,
changed heads before planning/after approval/during staging and application,
HTTP immutable-object tampering, replacement by another already granted channel
signer, selected dependency versus unused candidate
origins, upgrade refusal/retry, source deletion at the commit decision followed
by offline recovery/rollback, offline repair, 400 allocation-failure positions,
and native file/HTTP execution after freeing input objects. Native instrumented
runs exercise owned plan lifetimes rather than relying only on ctypes tests.
Local evaluation passes `make test-fusion`: 150 common methods plus the
existing contract/target/application/context/first-install/CLI/publish checks.
The sanitizer harness passes 70 methods, including native file/HTTP plan
lifetime/execution under ASan/UBSan; direct ctypes cases use the ordinary test
library. Documentation links pass for 67 files with zero broken links.
Exact-source Linux/macOS CI must separately qualify this checkpoint.

This is a checkpoint, not ALPHA-CHANNEL completion. Durable replay high-water
tracking, independent qualifications, default client integration and native HTTP
adapters remain open. No OS changes, AROS execution or live portal operation
occurred. Host process/recovery tests do not establish power-loss durability.

## FUSION-ALPHA progress: journaled channel replay floors

The transport checkpoint on source
[0a45f83](https://github.com/pomaros/apkg-fusion/commit/0a45f833a8e5608197d6ad410e439b2ee7e07097)
is now fully qualified by the
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40421306193).
The source-bound-plan checkpoint on
[188c63a](https://github.com/pomaros/apkg-fusion/commit/188c63afdfed0cf6d2f025681ac10b42e378933d)
is independently qualified by its full
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40422461420).

The [journaled replay profile](channels.md#journaled-replay-floors) adds explicit
approved acceptance through the common transaction engine. Roots retain sorted
channel identity/sequence/digest marks, capped at 1,024 identities. Reads remain
read-only, automatically reject older metadata and same-sequence forks, and
expose accepted sequence independently of unknown online freshness. Mirrors
share the same identity's floor. The common CLI previews/confirms file and HTTP
acceptance; it does not fetch/install a package or grant a signing key.

All package state builders preserve the registry. A separate package revision
allows rollback of the latest package history after intervening catalogue-only
acceptance, with exact current-root approval and strict recorded-history
validation. Rollback retains the current registry and consumes its package
revision, preventing repeated application of the same package history.

New tests cover persistence across contexts, stricter caller floors, forks,
old observations and racing approvals, file/HTTP mirrors, malformed stored
registries, sorted independent identities and uint64 boundaries, the 1,024-row
limit, package rollback after later acceptance, cancellation, five injected I/O
boundaries, process death and repeated recovery, 280 allocation-failure positions,
and actual CLI preview/confirmation through both endpoint types. Local
`make test-fusion` passes all 163 common methods and the existing integration
checks. The sanitizer harness passes 83 methods, including actual file/HTTP
acceptance in instrumented CLI binaries; direct ctypes tests use the ordinary
test library. Documentation links pass for 67 files with zero broken links.
Exact-source Linux/macOS CI must separately qualify this checkpoint.

A separate process-death probe at journal fault 1 confirms an existing
preparation-prefix defect: exit 99 leaves root bytes unchanged, but recovery
returns `(APKG_RECOVERY_REQUIRED, APKG_NEEDS_RECOVERY)` because `ready` was never
published. That checkpoint does not qualify preparation-prefix recovery. The
following journal checkpoint addresses new preparation prefixes; it preserves
the refusal for ambiguous preexisting active directories.

This is explicit acceptance, not automatic acceptance during default-client
refresh. Online revocation/freshness policy, independent qualifications, shared
client discovery/selection, native adapters and all other alpha gates remain
open. Host process tests do not qualify power loss or a live portal/AROS run.

## FUSION-ALPHA progress: private preparation and ready-journal publication

Both common executors now use the same private preparation helpers. They create
an exclusive mode-0700 sibling, write/flush intent, staging and identities, then
publish the complete ready journal without overwrite and flush its parent before
managed mutation. Readers observe the unchanged root until active publication;
the existing mutation lock still excludes competing writers. Ordinary errors
clean only the physically bound owned draft. Changed directory identity or a
foreign active coordinator refuses without deleting foreign bytes.

This closes the newly reproduced fault-1 root-blocking defect for new
transactions. Process death before active publication leaves only a private
orphan: root state is unchanged, recovery reports unchanged and an explicitly
approved retry can commit. After publication, the existing before/after-decision
recovery rules apply to a complete journal. Failed private cleanup reports
unchanged managed state with a diagnostic path. Unrelated orphans are preserved.
The [journal protocol](journal.md) owns the full contract.

New tests cover initialization, initial install, upgrade and channel acceptance
across five private-prefix death points, the publication/flush window, ready and
commit boundaries, normal I/O failure and retry. Deterministically paused host
processes verify read-only visibility, active-coordinator exclusion, replaced
private-directory refusal and unchanged personal files. Long-root filenames
retain their prior history-name budget. A native fault-enabled runner exercises
single/changes install cleanup and retry through instrumented binaries.
Local `make test-fusion` passes 172 common methods and the existing integration
checks. The sanitizer harness passes 92 methods, including the fault-enabled
native runner under ASan/UBSan; direct ctypes cases use the ordinary test library.
Documentation links pass for 67 files with zero broken links. Exact-source
Linux/macOS CI must separately qualify this checkpoint.

Preexisting incomplete or corrupt **active** journals still refuse recovery;
they are not guessed to be disposable drafts. Private orphan collection, native
handler behavior and power-loss durability remain unqualified. No AROS, OS or
live portal operation occurred, and default-client/qualification/import/portal
alpha gates remain open.

## FUSION-ALPHA progress: shared dependency-aware channel clients

The preceding replay-floor checkpoint on
[e97a9b9](https://github.com/pomaros/apkg-fusion/commit/e97a9b9c1b30beaeb167ebe343b2dfed0ee52df0)
is fully qualified by its
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40423890142).
The preparation-prefix checkpoint on
[99508f2](https://github.com/pomaros/apkg-fusion/commit/99508f2f0674c070c41cc46714012d8f29534978)
is independently qualified by its full
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40425031725).

The [shared client contract](channels.md#shared-dependency-aware-client-workflow)
now separates metadata resolution from payload preparation without introducing
a second solver. It discovers the relevant candidate closure, retains compatible
installed providers, backtracks through conflicts and fetches only selected new
archives. One canonical approval binds the discovered channel snapshot, exact
package, selected providers/source origins, package changes and replay floor.
Package/floor state commits in one common generation/journal.

`apkg-core channel-open` and the actual PkgManager discovery adapter share full
owned JSON facts. `channel-install` and the actual worker use the same apply
entry point. The native advanced panel exposes that operation; full interactive
catalogue/settings integration is still open. Worker progress messages preserve
all operation-id bits, event sequence, phase and 64-bit counters. Message
allocation failure now reports NOMEM rather than masquerading as user cancel;
the final preallocated completion still reports the real filesystem outcome.

New scenarios use real TOML/.ameta signed file/HTTP publication with multiple
provider versions. They compare CLI/worker discovery and installed records,
exercise installed-provider preference and conflicting-version backtracking,
withhold unselected archives, refuse missing metadata dependencies before any
archive request, bind the selected discovery snapshot before preparation, and
refuse unknown authors/targets or corrupt selected artifacts. Decline, cancel,
worker allocation failure and 500 planner allocation positions preserve root
state/floor. A real channel update preserves personal configuration, then rolls
back offline while retaining the newer floor; offline repair/removal also pass.
Local `make test-fusion` passes 186 common methods and the existing integration
checks. The sanitizer harness passes 106 methods, including real CLI/actual
worker file/HTTP workflows through instrumented native binaries; direct ctypes
cases use the ordinary test library. Documentation links pass for 67 files with
zero broken links. The subsequent exact-source
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40429056823)
passed on `517eda0484f03a96c2994a7083b6e3f6ee7754f7`, including both inherited
suites on Linux/macOS and all 107 portal tests.

This is one-channel host workflow integration. Default `build/apkg` replacement,
interactive native catalogue/settings, independent qualifications, multi-channel
configuration, legacy import, portal adaptation/rehearsal and target/runtime
qualification remain open. No live portal, OS or AROS execution occurred.

## FUSION-ALPHA progress: common engine becomes the default CLI product

The shared channel checkpoint on
[036705e](https://github.com/pomaros/apkg-fusion/commit/036705ea495bbbca1eccb8177a239c2392e66a4a)
is qualified by its full
[Linux/macOS/portal run](https://github.com/pomaros/apkg-fusion/actions/runs/40427067233).

`make` now selects common `build/apkg` and `build/libapkg.a`; `apkg-core` remains
byte-identical as a development alias. Default/install products exclude the
provisional request backend. The symbol gate checks the actual default binary
as well as the common library/alias. `build/apkg-provisional`, its platform
variants, `build/pkg` and the old library remain explicit regression targets.
Common macOS/static-Linux recipes use common sources; Windows product builds
explicitly refuse until common adapters exist. Those cross recipes have no new
runtime qualification from this checkpoint.

Author/build/channel helpers default to the product engine. `apkg-publish`
defaults to common publication; `--legacy` explicitly selects the preserved
old bridge, whose checks remain enabled. `make install` stages common tools,
minimal authoring resources, public headers/library and original licences,
with relative engine links and quoted PREFIX/DESTDIR paths. Installed authoring
works outside the source checkout. No operator installation was modified.

The default CLI now has common-engine identity/help. Testing also exposed the
prototype bootstrap's unusable cross-process digest confirmation (a fresh root
identity changed the plan). Initialization now retains its plan for exact-digest
confirmation in one terminal process. Nonterminal invocation previews only;
explicit `--yes` authorizes supplied bootstrap target/policy for automation.
Other operation confirmation tokens keep their existing exact-plan behavior.

The new default-product scenario generates keys, explicitly initializes clean
roots, publishes real TOML/.ameta bundles/dependencies without engine/mode
overrides, discovers through HTTP, compares CLI/actual worker results, updates
with personal configuration, then removes the channel and exercises rollback,
repair and removal. It checks source immutability, empty/hidden paths, generated
launch metadata, legacy-environment isolation, installed SDK linkage and all
installed authoring helpers under a prefix containing spaces. A real terminal
fixture tests approval/decline of the retained bootstrap plan. The provisional
CLI and old publishing regression scenarios remain separate, not replaced.
Local `make test-fusion` passes 186 common methods, 78 default-product checks,
110 retained provisional-CLI checks, 78 inherited publishing checks and the
existing contract/target/application/context checks. The sanitizer harness passes
106 methods through its instrumented native clients/runners; direct ctypes
cases use the ordinary test library. Documentation links pass for 67 files with
zero broken links. Exact-source Linux/macOS CI must qualify this checkpoint
separately.

This advances the default CLI, not the entire ALPHA-CLIENTS/release gate.
Interactive native catalogue/settings, independent qualifications, validated
legacy import and portal adaptation/rehearsal remain open. No live portal or
AROS/OS operation occurred.

## FUSION-ALPHA progress: typed catalogue selection

Interactive clients now have `apkg_client_channel_open` and
`apkg_client_channel_selection` in the installed client API. JSON discovery
uses that same read-only opening path. The channel owns complete typed rows
after the temporary context closes; row selection returns the observed
channel/snapshot/descriptor identities without projecting through the inherited
63-character PkgManager name field. Selection itself grants no authority and
downloads no archive. Applying it still uses the shared dependency-aware plan
and refuses a changed channel head.

The host browser cases cover two distinct 255-character names, full typed
facts, read-only HTTP discovery, out-of-range selection, allocation failure
and retry, pre-cancellation, dependency installation and changed-head refusal.
The native memory harness exercises view/selection ownership after context
closure and is included in the sanitizer run. This is an API integration step;
interactive PkgManager row selection, settings and native runtime qualification
remain open.

Batch evaluation: `make test-fusion` passed (190 common methods at collection,
78 default-product, 110 provisional CLI and 78 publishing checks, plus the
existing contract/target/application/initialization gates). The final five-case
browser suite passed separately, including its subsequently added cancellation
case. `tests/apkg/sanitize.sh` passed all 111 selected methods with instrumented
native CLI/worker/memory harnesses; direct ctypes cases use the ordinary test
library. Documentation links cover 67 files with zero broken links. Exact-source
Linux/macOS CI remains the subsequent qualification gate. No native execution
or portal changes were performed in this batch.

## FUSION-ALPHA progress: native catalogue discovery and selection wiring

The fusion PkgManager has a separate signed-catalogue window with endpoint and
channel identity, worker discovery, full-name/authority/target search, details,
channel snapshot/floor/trust presentation and selected install/update review.
The selection calls the same common plan/confirmation path already used by the
CLI and mutation worker. Other-target and untrusted-author rows stay visible
with review disabled; review and runtime qualifications are explicitly unasserted.

Discovery transfers one bounded `AllocVec` model to the UI. It preserves
255-character names/versions, full unsigned 64-bit revisions, exact selections
and owned channel evidence after worker library handles are closed. Failure
leaves the previous displayed catalogue intact. UI-owned cancellation tokens
remain alive until completion, including during transport waits; closing the
catalogue window cancels discovery. Ignored and shutdown messages release their
owned catalogue copies.

This closes source wiring for interactive discovery/selection, not the full
native client gate. Installed-list projection, settings, news/update views and
native interaction remain outstanding. The AROS SDK header/type check also
found the DOS varargs inline's explicit `alloca` feature requirement in bundle
launch; the native-only include now requests it, without enabling GNU stdio
extensions. No OS repository or installed updater was changed.

Batch evaluation: `make test-fusion` passed all 194 common methods, the
78 default-product checks, 110 provisional CLI checks, 78 publishing checks and
existing contract/target/application/initialization checks. The sanitizer run
passed 114 selected methods, including actual worker copies with 255-character
names/versions and maximum unsigned 64-bit revisions, blocked-author/target
facts, empty catalogues, progress/model allocation failures, cancellation and
discovery-to-install selection. Native clients/runners are instrumented;
direct ctypes cases use the ordinary test library. The expanded
`tools/check-apkg-aros.sh` passed header/type checks with the existing aarch64
AROS SDK (its `vfork` declaration warnings remain). Documentation links pass
for 67 files with zero broken links. These are host/type checks, not a native
UI execution result; exact-source Linux/macOS CI remains the next gate.

## FUSION-ALPHA progress: full typed installed rows

PkgManager's fusion installed list no longer projects through `pkg_entry`.
Its bounded, owned `AllocVec` model copies every text fact and evidence field,
record bytes, instance identity, reason and signature/trust state from the
common snapshot before closing the context. Names and versions retain 255
characters, revisions retain all unsigned 64 bits, and search covers complete
names, authorities and targets. Unknown revision/version facts remain labelled
unknown. Non-ready roots are refused without initialization or legacy writes.

The fusion list hook/details, selection preservation and Remove/Open/Run
actions use those typed rows. Worker and ARexx job names now retain the common
name bound; the fusion ARexx tokenizer accepts the complete name and refuses
overlong input. The inherited parser retains its original limits when built
without fusion. Installed LIST/SHOW and RUN/REMOVE source wiring use common
reads/actions; remaining legacy ARexx queries explicitly refuse while their
adapters are pending, preventing `pkg_open` from creating a second database in
the common root. This is not complete ARexx/client compatibility qualification.

Five host cases exercise the actual native adapter with distinct 255-character
names/versions, maximum revisions, all copied evidence pointers owned by the
returned allocation, empty/absent/legacy/pending roots, allocation failure and
retry, full-name search, repair/removal of the selected package and Run reaching
the host's explicit unsupported-runtime boundary. No AROS executable is run.
The existing ARexx corpus now also runs with fusion fields, including longest
accepted/rejected name boundaries and untruncated job formatting.

Batch evaluation: `make test-fusion` passed 199 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer run passed 119 selected methods plus the instrumented
fusion ARexx corpus; native workers/runners are instrumented, while direct
ctypes cases use the ordinary test library. The expanded SDK check passed on
the existing aarch64 AROS headers, with the previously noted SDK `vfork`
warnings. All 67 documentation files pass link checks. ARexx/UI dispatch has
source/type evidence, not native runtime evidence. Exact-source Linux/macOS
CI remains required, alongside both unchanged inherited regression suites.

## FUSION-ALPHA progress: shared installed inspection and INFO

The common library now owns `apkg_snapshot_inspect`, an immutable independent
inspection of a committed installed record. It verifies the captured descriptor
signature and evaluates its signer against captured author grants. Policy
withdrawal, invalid signature, root-target agreement and independent
qualifications remain separate facts. It exposes full metadata/relations,
declared file/directory/byte counts and root/generation/snapshot provenance.
Live payload verification, runtime requirements, review/testing evidence and
upgrade/rollback availability remain unperformed/unknown rather than inferred.

`apkg info ROOT NAME [FIELD]`, native installed details and fusion ARexx INFO
share the formatter and exact-name read path. The native ARexx adapter refuses
oversized answers without truncation and supports smaller field queries.
Fusion advertises ARexx interface 2 for the explicit unknown and declared-target
semantics; the imported non-fusion client retains interface 1. The
[inspection profile](inspection.md) records those meanings and limits. This
does not complete runtime REQUIREMENTS, catalogue-only INFO or all ARexx
compatibility adapters, and supplies no independent review attestations.

Seven host cases cover CLI/native read-adapter equivalence, independent lifetime
after snapshot/context close, captured-policy versus subsequent withdrawal,
invalid signatures, edited live files, target mismatch, full 255-character and
unsigned 64-bit values, all relation data, pending/absent/legacy roots, byte-sum
overflow, allocation-failure/retry and the 65,535-byte ARexx boundary. A native
instrumented harness reads/formats owned inspection views after freeing their
inputs. Native UI/ARexx dispatch still needs runtime qualification.

Batch evaluation: `make test-fusion` passed all 206 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer run passed 126 selected methods plus the instrumented
fusion ARexx corpus. Host C clients/read-adapter lifetime harnesses are
instrumented; direct ctypes allocation-injection cases use the ordinary test
library. The aarch64 AROS SDK header/type check passed with the previously
noted SDK `vfork` warnings. Documentation links pass for 68 files. Exact-source
Linux/macOS CI and native runtime qualification remain separate gates.

## FUSION-ALPHA progress: shared update comparison and catalogue filters

`apkg_channel_compare` now compares exact committed descriptor digests,
authority/name/target, common version ordering and unsigned revisions. It
distinguishes current/newer/older, changed same-release content, indeterminate
ordering and owner/target conflicts. Readiness remains unknown until an actual
plan. Installed descriptor hashes are captured during the metadata read, not
copied from an envelope's claimed hash. A changed root/policy/target or replay
floor refuses a mismatched observation.

The new `apkg_client_channel_refresh` returns a compatible owned root/channel
pair and aligns accepted-floor facts with that captured root. The CLI
`channel-updates` report and actual native discovery worker use this common
comparison. PkgManager copies installed version/revision and comparison into
its owned catalogue, with filters for updates/blocked candidates, not installed,
other targets, untrusted authors, content conflicts and indeterminate versions.
Blocked rows remain visible with their owner/target details; metadata permitting
review still needs the shared resolution/confirmation/journal path.

Batch review found and corrected first-match ambiguity: qualified comparison
now finds the matching owner/target even when a foreign same-name record comes
first. Duplicate exact identities or multiple unmatched owners refuse. The
unqualified INFO/folder/Run/remove/repair/upgrade paths refuse multiple names
without touching records or payload; explicit index inspection stays available.
The [comparison profile](updates.md) records these contracts. This does not
complete legacy OUTDATED aggregation, settings, dated news or native runtime
qualification.

Seven added host cases cover full identities/values, version and content
classes, policy/root/floor refusal, CLI/actual worker/filter equivalence,
trusted foreign-owner separation, ambiguous lookup refusal, allocation/retry
and installation of a selected real newer archive followed by a current-state
refresh. Neither discovery nor comparison fetches unselected archives or
accepts a channel floor implicitly.

Batch evaluation: final `make test-fusion` passed 213 common methods, the
fusion ARexx corpus, 78 default-product checks, 110 provisional CLI checks,
78 publishing checks and existing contract/target/application/initialization
gates. After the ambiguity correction, the sanitizer suite passed 133 selected
methods plus the instrumented fusion ARexx corpus. Host C clients/workers and
owned-lifetime harnesses are instrumented; direct ctypes fault-injection cases
use the ordinary test library. SDK header/type checks passed on the existing
aarch64 AROS headers with the previously noted `vfork` warnings. Documentation
links pass for 69 files. Exact-source Linux/macOS CI remains the next gate;
native runtime qualification is still pending.

The prior inspection checkpoint `1262c28bed556c1cfec1bb37c8810d20634a6a7c`
has now passed its complete
[exact-source CI](https://github.com/pomaros/apkg-fusion/actions/runs/40433664147),
including both inherited suites on Linux/macOS and all 107 portal tests. That
result does not qualify this subsequent comparison/hardening change.

## FUSION-ALPHA progress: package summaries, UPDATE and OUTDATED

The common `apkg_channel_summarize` now counts qualified installed packages
separately from offered builds. It reports known newer packages, metadata
eligibility/blocking, conflict/indeterminate candidates and absence from this
observed source. It owns complete qualified entries/versions/revisions after
its input objects close. Duplicate identities and ambiguous bare-name lists
refuse; scalar and qualified fields remain available. Plan readiness and online
freshness stay unknown, and missing source coverage is not called withdrawal.

`apkg outdated ROOT ENDPOINT CHANNEL-ID [FIELD]` shares the formatter with
fusion ARexx OUTDATED. The actual discovery worker deep-copies the view, entry
array and text into its owned catalogue, which displays observed generation
and distinct metadata/readiness counts. `OUTDATED SHOW` opens its update filter.
`UPDATE`/Refresh use common signed discovery rather than the old index writer;
the imported non-fusion command corpus retains interface 1 behavior.

The cached native query makes no network request and refuses changed/pending
root observations instead of claiming current counts. Result-size overflow
refuses without truncation; individual fields remain usable. Persistent source
settings and offline cached-source re-evaluation after root changes remain
required integration work, alongside dated news and runtime qualification.

Six new host cases cover logical-package counts across multiple versions and
keys, metadata/readiness separation, qualified/unqualified ambiguity, empty
sources/coverage, large ARexx answers, owned lifetimes, allocation-failure retry
and stale/pending root refusal with no query-time network. The host C harness
checks all copied summary strings belong to the native allocation and formats
the engine object after its snapshot/channel inputs have been freed.

Batch evaluation: `make test-fusion` passed 219 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer suite passed 139 selected methods plus the instrumented
fusion ARexx corpus; host C clients/workers/owned-copy harnesses are instrumented,
while direct ctypes fault-injection cases use the ordinary test library.
SDK header/type checks passed on the existing aarch64 AROS headers with the
previously noted `vfork` warnings. All 69 documentation files pass link checks.
Native ARexx/UI dispatch still has source/type evidence rather than execution
evidence. This source revision still needs its exact Linux/macOS CI gate.

The preceding comparison checkpoint `bcf6a65478d0e7970d4d21d543efa999a3b37b18`
has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40436282928),
including both inherited suites on Linux/macOS and all 107 portal tests.

## Offline re-evaluation of signed catalogue observations

The native model now retains the exact channel object and signature in its
single bounded `AllocVec` allocation. A public borrowed-byte accessor exposes
these observations for copying without exporting an install/transfer origin.
Library handles still die on the worker; the copied signed pair survives with
the model. Allocation accounting includes both spans and refuses over-limit
models before allocation, including the summary/evidence terminators.

Cached native OUTDATED revalidates that pair through `apkg_channel_read` in the
current read-only context, then computes a new common summary against a fresh
installed snapshot. Removing a package therefore changes the answer offline.
Current author grants and root target affect metadata eligibility; withdrawn
channel grants, pending transactions, old sequences and same-sequence forks
refuse explicitly. The result still asserts neither online freshness nor plan,
review or runtime readiness. No root or source cache is written.

The expanded host corpus exercises eight post-discovery changes over HTTP with
no additional requests: generation, a journaled package removal, author/channel
grant withdrawal, root target, journaled higher replay floor, journaled fork
acceptance and pending transaction. It verifies exact answers and unchanged
committed bytes across the query. A new observation-lifetime case checks exact
exported bytes, successful re-reading after owner destruction, invalid outputs
and refusal of tampered object/signature bytes. Native allocation checks now
cover the copied signed pair as well as all summary strings.

The displayed immutable rows retain their observed generation. SHOW opens that
view; the query does not silently relabel old UI rows as refreshed. Persistent
source settings, durable observation caching and offline row-refresh dispatch
remain required work. This checkpoint is not a completed native settings or
runtime gate, and changes neither the OS repository nor installed M1 updater.

Batch evaluation: `make test-fusion` passed 220 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer suite passed 140 selected methods plus the instrumented
fusion ARexx corpus; host C clients/workers/owned-copy harnesses are instrumented,
while direct ctypes fault-injection cases use the ordinary test library.
SDK header/type checks passed on the existing aarch64 AROS headers,
with the previously recorded `vfork` warnings. All 69 documentation files pass
link checks. Native UI/ARexx runtime remains untested; persistent settings and
row-refresh integration remain open. Exact-source Linux/macOS CI is required
for this checkpoint before treating it as host-qualified.

## Offline catalogue row refresh and SHOW consistency

The native catalogue now exposes Recompare offline through a separate common
worker job. Its input is a borrowed immutable UI-owned AllocVec, retained until
PM_DONE; its result is an independent AllocVec containing revalidated signed
metadata, complete rows/details, exact selections and a new root-bound summary.
The UI clears list/detail references before replacement. Failed/cancelled work
preserves the old observation and its generation. Cancellation is checked at
metadata-rebuild boundaries rather than inside signature/parser calls.

`OUTDATED SHOW` uses the same model builder and formats its replacement summary
before updating the window. Returned fields and visible row facts thus share a
single captured snapshot. Invalid fields, result overflow and refusals discard
the tentative model. SHOW refuses while an operation holds the UI model;
ordinary scalar queries do not replace it. No network request, archive transfer,
trust grant or managed-root write is introduced by either refresh path.

An additional host case runs nine real worker/SHOW scenarios after discovery:
package removal, author withdrawal, target change, higher replay floor,
channel withdrawal, invalid field, cancellation and native allocation failure.
It checks current generation, every selection's original source digest, all-row
trust/target facts, changed package comparison and zero extra HTTP requests.
The C harness compares the entire input allocation before/after, checks owned
output rows and frees the original before inspecting the independent result.
Persistent source settings, durable observation caching, dated news and native
UI/runtime qualification remain open; this closes only the offline row-refresh
integration gap identified in the preceding checkpoint.

Batch evaluation: `make test-fusion` passed 221 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer suite passed 141 selected methods plus the instrumented
fusion ARexx corpus; native C worker/model harnesses are instrumented, while
direct ctypes fault-injection cases use the ordinary test library. All 69
documentation files pass link checks. The final native sources passed SDK
header/type checks on the existing
aarch64 AROS headers with the previously recorded `vfork` warnings. These
checks establish types rather than UI dispatch or runtime behavior. This
checkpoint still requires exact-source Linux/macOS CI.

The earlier summary checkpoint `f0a3d8ed9ed0119ecfc651b4f809f61fb73e15e5`
has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40439424137),
including both inherited suites on Linux/macOS and all 107 portal tests.

## Persistent source profiles and offline restart

The common client now owns [source profiles](sources.md): an explicit endpoint,
channel identity and optional exact signed observation, in a bounded private
file outside managed-root metadata. Settings restore without a root or trust
grant. Opening retained metadata always revalidates it against current root
policy, target and replay floor. Returned snapshot/channel owners remain usable
after the profile owner/context are freed. No transfer origin, readiness or
online freshness is inherited from a file's presence.

The default common CLI exposes settings save, discovery/cache save, profile
inspection and offline OUTDATED. PkgManager exposes a path control, explicit
save/restore and startup restore of existing profiles; the shared worker builds
a complete independent row model from retained metadata without network access.
Saving retains displayed bytes only if endpoint and channel identity match the
controls and current-root validation succeeds; changing a source saves settings
only. Browsing and exit do not implicitly write profiles or accepted floors.

Six host cases cover private settings-only files/cache clearing, HTTP discovery,
CLI/native save byte equality, restart with the server stopped/source removed,
CLI/actual-worker answer equivalence, journaled removal/replay-floor changes,
withdrawn author/channel roles, forks, pending roots, framing/signature tampering,
truncation/oversize, symlink/FIFO refusal, allocation failure and cancellation.
Profile and root bytes remain unchanged on read/refusal; write validation and
pre-replacement failures preserve existing profiles. Host file-service atomic
replacement is process-visible evidence, not power-loss or native durability.

Multi-channel aggregation/precedence, automatic retention and native settings/UI
runtime qualification remain open. The native filesystem adapter refuses writes
when atomic replacement is unsupported; no older-DOS fallback or native runtime
success is claimed. Neither the OS repository nor the installed M1 libpkg/update
process is changed.

Batch evaluation: `make test-fusion` passed 227 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates. The sanitizer suite passed 147 selected methods plus the instrumented
fusion ARexx corpus; host C CLI/worker/model harnesses are instrumented, while
direct ctypes fault-injection cases use the ordinary test library. Public
library symbol checks passed. The native sources passed SDK
header/type checks on the existing aarch64 AROS headers with the previously
recorded `vfork` warnings. All 70 documentation files pass link checks. The
native startup/control wiring has not been executed on AROS; this checkpoint
still requires exact-source Linux/macOS CI, including the inherited suites.

## Portable signed review/runtime observations

The common engine now verifies [attestations](attestations.md) against exact
signed descriptors and captured root policy. Optional reviewer/tester grants
remain separate from author/channel roles and are displayed during approved
initialization. Reports retain passed/failed/revoked claims, explicit issuer
trust/independence, target/environment, evidence, complete 64-bit values and
snapshot provenance. An author's key alias or another key of its authority
cannot satisfy independence. Review privileges cannot substitute for testing.

Record histories can link an explicit contiguous predecessor prefix by digest,
issuer/kind/content, sequence and claimed timestamp; runtime links preserve
hardware/OS-build/test-suite scope while allowing new log bytes. Missing history remains
unchecked and freshness always unknown: a prefix or future-dated signature is
not a current approval. Object owners survive their inputs, snapshot/context
and predecessor owners. No payload/environment test or evidence fetch occurs.

The default CLI signs/verifies this domain and inspects a claim folder through
a shared formatter. PkgManager's explicit read-only inspection job uses the same
client helper, returns an owned native message text and displays the claim; no
malloc-owned handle crosses worker exit. Seven host cases cover CLI/actual-worker
equivalence, signing/domain confusion, exact content/identity, runtime environment,
author/channel/role separation, key aliases, withdrawal/target facts, history
links/revocation/missing prefix, maximum fields, ownership and allocation failure.
Host-harness cancellation/AllocVec failures and pending roots refuse without writes.

This implements portable observation verification, not the entire qualification
gate. Channel carriage/publication, native row/filter projection, retained
revocation-head observation and portal adaptation remain required. No policy
requiring current independent approval is silently accepted; no native runtime
or independent test execution is asserted by this checkpoint.

Batch evaluation: `make test-fusion` passed 234 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78
publishing checks and the existing contract/target/application/initialization
gates after the runtime-history scope correction. The sanitizer suite passed
154 selected methods plus the instrumented fusion ARexx corpus; host C
CLI/worker/claim-history harnesses are instrumented, including predecessor/input
release and runtime-scope links, while direct ctypes fault-injection cases use
the ordinary test library. Public library symbol checks passed. Final native sources passed SDK header/type checks on the existing
aarch64 AROS headers with the previously recorded `vfork` warnings. All 71
documentation files pass link checks. Native controls/dialogs and late UI
cancellation have source/type evidence, not AROS execution evidence. This
checkpoint still needs exact-source Linux/macOS CI and inherited-suite gates.

The preceding row-refresh checkpoint
`1da8aa5cd2b9f30b5723602a1e85a7e01705ee03` has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40440800308),
including both inherited suites on Linux/macOS and all 107 portal tests.

## Signed observation carriage, publication and catalogue rows

Channels now carry bounded per-entry [attestation sets](attestations.md#channel-carriage-and-catalogue-presentation).
The common validator checks embedded signatures, exact descriptor binding,
canonical set order, duplicate/fork conflicts and supplied history links.
Preflight uses signature/schema facts only; discovery computes role/independence
facts from a real captured root snapshot. No synthetic root approval is exposed.
Library getters retain wire order, complete signed bytes, captured provenance
and unchecked history/freshness where evidence is missing.

The file-channel publisher accepts explicit claim folders, verifies their
binding against the actual archive and preflights the set before endpoint
creation. Repeated publication preserves prior observations; new claims produce
a new immutable snapshot, exact repeats remain unchanged, and conflicting
signatures for one record digest refuse instead of replacing provenance. New
content receives no observations by name/version similarity. Author publication
and installation remain independent of reviewer authorization.

Discovery and updates JSON expose each raw record/envelope and separate policy,
independence, target, prefix and freshness facts. The PkgManager worker copies
the shared complete formatted evidence into its existing bounded native model.
New review/runtime/revocation observation filters never assert current approval.
Saved source profiles and offline model rebuilds revalidate the same complete
signed channel. The common variadic text builder now has compiler format checks;
the targeted suite found and fixed a presentation argument mismatch before push.

Five new host cases exercise real TOML archive publication, append/idempotence,
immutable prior heads, HTTP CLI/worker presentation, cached/offline restart,
role withdrawal, self-reports and unreviewed installation, content separation,
signature/identity/history/order/fork refusals, signature replacement refusal,
64/65-record limits, owner lifetimes and allocation-failure cleanup. Host workers
check owned detail strings and all three observation-filter counts after library
handles are gone. No additional archive requests occur during discovery/restore.

This is not current qualification/revocation tracking across sources: omitting
known evidence from a later source and obtaining fresh revocation observations
still require their own policy/state work. The new filters inspect observations;
no reviewed/tested approval requirement is silently enabled. Portal adaptation,
native UI/runtime and release qualification remain open.

Batch evaluation: the final `make test-fusion` passed 239 common methods,
the fusion ARexx corpus, 78 default-product checks, 110 provisional CLI checks,
78 publishing checks and the existing contract/target/application/initialization
gates. Legacy native-harness assertions were updated to check explicit current
approval remains unasserted; ownership and exact-selection checks remain active.
The final sanitizer suite passed 159 selected methods plus the instrumented
fusion ARexx corpus; host C CLI/worker/model/history harnesses are instrumented,
while direct ctypes fault-injection cases use the ordinary test library.
Public library symbol checks passed. Native sources passed SDK header/type checks
with the existing aarch64 AROS headers and previously recorded `vfork` warnings.
All 71 documentation files pass link checks. Native row/filter execution remains
unqualified; exact-source Linux/macOS CI still must qualify this checkpoint.

The preceding portable-claim checkpoint
`85f369b6914394816ccdf8675e85bcf6f7dc9f98` has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40443095605),
including both inherited suites on Linux/macOS and all 107 portal tests.

## Journaled accepted observation memory across sources

The common root now retains [accepted qualification records](qualification-memory.md)
in the same state/journal as packages and channel floors. Explicit approved
channel acceptance and selected channel installation add exact signed records
only for currently authorized independent reviewers/testers. Reads never commit
observations. All earlier pairs are retained, with duplicate/fork/signature and
history consistency checks and strict metadata/count bounds; old roots normalize
missing memory to empty. No second mutation engine or recovery model is added.

Discovery re-associates retained evidence by exact descriptor digest, verifies
its signatures and current role/independence facts, and appends omitted evidence
after supplied observations. Origins, accepted sequence/digest and
older-than-accepted facts remain explicit. A source omitting a revocation, an old
positive cache or an unauthorized higher sequence cannot erase this local memory.
Role withdrawal changes current trust facts without rewriting accepted history.
No observed sequence or passed claim implies online freshness/current approval.

The common CLI and actual worker share explicit channel-acceptance behavior and
preview retained identities/results before confirmation. Metadata acceptance
compares/preserves exact installed records and skips payload/relation changes,
including for withdrawn publisher grants and personal file edits. PkgManager exposes that
shared action alongside its signed package actions. Selected installation records
packages, source floor and qualified evidence in one plan. Package operations
preserve records; package rollback across later channel acceptance keeps current
memory. Recovery remains repeatable and offline on both sides of the decision.

Seven new host cases cover cross-source omission/HTTP/offline cached projection,
decline and CLI/worker acceptance equality, unauthorized counters/withdrawal,
content/fork/signature/corruption refusal, direct/selected installs and repair/
removal, rollback across later acceptance, process death/interrupted recovery and
allocation-failure cleanup. Remote freshness/expiry and newly published revocation
discovery, current-approval policy, reset/rotation, portal adaptation and native
runtime/power-loss qualification remain open. The installed M1 updater is untouched.
The allocation campaign also corrected pre-existing planner/history error mapping
and prevents a failed unchanged-entry comparison from producing payload work.

Batch evaluation: the final `make test-fusion` passed 246 common methods,
the fusion ARexx corpus, 78 default-product checks, 110 provisional CLI checks,
78 publishing checks and the existing contract/target/application/initialization
gates. The final sanitizer suite passed 166 selected methods plus the instrumented
fusion ARexx corpus; host C CLI/worker/model/history harnesses are instrumented,
while direct ctypes allocation faults and forked journal faults use the ordinary
test library. Public library symbol checks passed. Final native sources passed SDK
header/type checks with the existing aarch64 AROS headers and previously recorded
`vfork` warnings. All 72 documentation files pass link checks. Native menu/runtime,
handler durability and power-loss behavior remain unqualified; exact-source
Linux/macOS CI still must qualify this checkpoint and both inherited suites.

The preceding channel-carriage checkpoint
`2ed08ce81ae2f516dc67ba33432bbc84f03bf560` has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40445111372),
including both inherited suites on Linux/macOS and all 107 portal tests.

The observation-memory checkpoint `e7ad01312986e47d5df7d556318e2fa02a86b9d4`
has passed its complete [CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40448150525):
Linux/macOS common and inherited suites, memory checks, symbols/documentation and
all 107 portal tests.

## Existing-bundle adoption and persistent ownership guards

[Adoption](adoption.md) adds explicit registration through the common planner,
CLI and actual PkgManager worker. A trusted supplied artifact authenticates its
signed baseline; matching historical file hashes/sizes bind recorded ownership.
Raw source records and unknown installation reason remain local evidence.
Original legacy authentication stays unknown and is shown separately from the
supplied artifact's valid signature. No synthetic publisher or author grant is
created. The native menu exposes the shared action; native execution is untested.

The adoption transaction preserves the full observed tree. Local ownership guards
protect edits, unknown files (including bytes identical to the artifact), hidden
files and empty directories through repair, direct/resolved updates and removal.
The same immutable preconditions and journal implement stale-plan refusal,
interruption/repeat recovery and rollback to an unmanaged bundle. Missing recorded
files can be explicitly repaired from the actual signed baseline. An existing
managed owner or mismatched/invalid recorded baseline refuses before commit.

Nine host tests cover these behaviors, CLI/actual-worker state equality and
inspection, root/file AROS attributes and unsupported-node/case-alias refusal,
plus 1,000 injected allocation failures each for adoption planning, adopted-state
reading and repair planning. The tree-merge correction also preserves existing
bundle-root attributes in ordinary operations, including observation, staging and
journal recovery. The final `make test-fusion` passed 255 common methods,
the fusion ARexx corpus, 78 default-product checks, 110 provisional CLI checks,
78 publishing checks and the existing contract/target/application/initialization
gates. The final ASan/UBSan run passed 167 selected methods and the instrumented
ARexx corpus. Its actual CLI/worker adoption scenario also repairs, upgrades and
removes the adopted bundle; direct ctypes/fork allocation and journal faults use
the ordinary test library. Public symbols and links across 73 documentation files
pass. Final native sources pass the aarch64 AROS SDK header/type check with the
existing `vfork` warnings. Exact-source Linux/macOS CI and both complete inherited
suites remain required for this checkpoint; native runtime and power-loss
qualification remain open.

This is a common-engine prerequisite, not the completed legacy importer: legacy record mapping, original-preserving copies, settings/history,
interruption/compatibility rehearsals and distribution evidence remain required.
The M1's installed libpkg updater remains owned by another migration effort.

The existing-bundle adoption checkpoint
`8621185091b75b953b4e7b8db4fee8e936266d5d` has passed its complete
[CI run](https://github.com/pomaros/apkg-fusion/actions/runs/40451055119),
including Linux/macOS common and inherited suites, memory/symbol/documentation
checks and all 107 portal tests.

## Schema-1 legacy copy import rehearsal

The [host adapter](legacy-copy.md) inventories a quiescent arospkg root, creates
an out-of-place copy, retains original database bytes under `.apkg-legacy`, and
previews one exact common plan. Its immutable receipt binds complete source/copy
inventories, physical roots, explicit artifacts and policy, target and original
observations. Confirmation refuses changed inputs; no original/root-global
settings or installed M1 updater are changed. Only the common engine initializes
policy, changes installed records and recovers mutations. A partial preparation
is preserved and retried into a fresh destination, never overwritten.

Common `apkg_plan_adopt_many`, CLI `adopt-set` and the actual PkgManager worker
share an atomic adoption set, including cyclic dependency cases. Context bounds
cover counts and aggregate captured trees. Migration compares verified selected
identity/version/revision/CPU/recorded ABI and baseline hashes/sizes. Original
signatures remain unknown. Missing ABI/reason/relations and effective revision
zero retain separate provenance. Common descriptors and dependency floors now
accept revision zero; it is not replaced with an invented packaging revision.
The rehearsal also corrected rollback of a removal whose unowned/personal bundle
contents remain: the final plan comes from verified retained before/after trees,
while ordinary installation still refuses taking over a foreign bundle.

Fourteen new host cases exercise pinned current/omission fixtures, original-byte
preservation, edited configuration, root attributes, hidden/empty paths, unowned
Workbench icons/settings, missing prior history, signature/build mismatch,
unsafe/mixed/pending legacy inputs, exact-approval input changes, CLI/worker set
equality, cyclic sets, zero revisions/floors and 1,200 set-planning allocation
failures. Preparation death/fresh retry and four common commit/recovery death
boundaries preserve the original; decided recovery and committed retry work with
source/artifacts offline. An unrelated pending plan refuses automatic recovery.
The imported copy supports common inspection, repair, removal and offline
rollback. The final `make test-fusion` passed 269 common methods, the fusion
ARexx corpus, 78 default-product checks, 110 provisional CLI checks, 78 publishing
checks and the existing contract/target/application/initialization gates. The
final ASan/UBSan run passed 176 selected methods plus the instrumented ARexx
corpus. Host CLI/worker processes are instrumented; direct ctypes allocation and
forked common-journal faults use the ordinary test library. The signed PKG/1
archive variant also passed a final targeted ordinary/instrumented run after its
addition to the omission-fixture case. Public symbols and all 74 documentation
files pass checks. Final native sources pass aarch64 AROS SDK header/type checks
with the existing `vfork` warnings. Exact-source Linux/macOS CI and both complete
inherited suites remain required for this checkpoint.

The profile explicitly refuses nonempty legacy relations/system requirements,
owned shared icons and unknown record fields. It retains launch/settings and
prior-version history as source evidence, without activating or converting them.
External associations, old script/command behavior, broader profiles and actual
distribution/native/DOS/power-loss evidence remain required. These fixture passes
do not complete ALPHA-IMPORT or the full alpha goal. Portal work remains deferred
until the other workstreams and isolated M1 rehearsal.

## Independent review and single-root self-update

Three read-only reviews covered the journal and recovery, channel trust, and
untrusted payload handling. Trust verification was found consistent; the
journal and payload reviews found defects that are corrected here:

| Defect | Correction and evidence |
|---|---|
| AROS posixc st_ino is a path hash and st_dev a DOS list address, so every moved tree would fail its identity check and leave the root pending | Identity comes from `ak_stat`: device/inode on hosts, the `Examine()` object key and the volume name and date on AROS; no object key refuses mutation. Host suites pass; AROS is type-checked only |
| Initialization recovery published into a root created by someone else after the decision | Recovery follows the recorded anchor; a new test leaves the foreign root and journal untouched |
| Recovery followed a linked `.apkg` and rewrote state outside the root | Linked metadata or state blocks recovery; tested |
| A failing existence check during rollback was treated as absence and reported success | Unknown existence is an error |
| A failed rebind cleared the context binding; `Work:Packages` resolved against the current directory on AROS | Rebind changes outputs only on success; AROS volume roots split at the colon |
| Payload paths of 256 bytes or more never matched, so such packages could not be repaired or removed | Token comparison has no length limit; a 264-byte path repairs and removes |
| One shared blob could expand without bound | The total declared size of a descriptor is capped at the PKG/1 envelope limit |
| 64 KiB stack buffers and 12 KiB per tree level overran a 256 KiB AROS stack | Buffers and path scratch are heap memory; the AROS CLI moves to a 1 MiB stack |
| Retained attestations were signature-checked on every root read | A context skips re-verifying the byte-identical record set it already verified; changed bytes are verified again |
| A descriptor revision parse failure could return success with no package | It returns INVALID |
| Incomparable dependency candidates were chosen by string or digest order | The channel plan refuses unless the package is the explicit selection |

Single-root self-update is described in the
[journal design](journal.md#single-root-self-update). Ten host cases cover
current, check-only, decline, update and offline rollback, an edited client,
an unowned or outside client, an unsettled legacy journal, recorded copies, a
release without the client at the same path, the CLI confirmation round trip,
crashes before and after the decision, and the actual PkgManager worker
bridge checking and then updating through its confirmation message.

## Trust administration

An explicit [policy change](channels.md#changing-the-policy) replaces the root's
trust policy through the common journal. Seven host cases cover a rotation
whose preview names every granted and revoked key and the installed package
losing author trust, an added reviewer role, refusals of unchanged, invalid,
non-canonical and read-only requests, a stale plan, rollback that never crosses
the change, crashes before and after the decision, and the CLI confirmation
round trip. The self-update and policy cases also run under ASan/UBSan.

## Root source list

The [root source list](channels.md#root-source-list) carries pkg's ordered,
named channel list into the common model. The channel planner takes several
channels in precedence order; the single-channel API is the one-channel case.
Six host cases cover preview, ordered listing and refusal of untrusted,
duplicate and badly named sources; the newest version across sources with only
the supplying floor advancing and an unchanged second run; an equal version
from the earlier source; a dependency from another source recording both
floors; refusals for two authorities, an unknown name and an unreadable
source; and removal that keeps installed packages and is never rolled back.

## Recorded legacy relations

The import maps recorded legacy relations instead of refusing them; see the
[import profile](legacy-copy.md#profile-and-qualification-boundaries). Two
host cases import a dependent pair with a recommendation and its dependency
together, and refuse a different signed minimum, an unreadable id, a
conflicting family and a self-named relation with the original unchanged.
Recorded system requirements follow the same rule; see
[system requirements](#system-requirements).

## System requirements

Signed [system requirements](data-model.md#targets-and-system-requirements)
replace arospkg's unsigned `requires_system` index field. Eight engine cases
cover a missing and a too-old library refusing before any change, satisfied
and undetermined observations bound into the plan and shown, a package without
requirements planning unchanged, an approved plan going stale when the machine
changes, an upgrade probing the new release, eight malformed declarations,
crashes before and after the decision, and TOML authoring through to a host
plan that reports undetermined. Two import cases accept a recorded set equal
to the signed one and refuse a different minimum, an undeclared signed
requirement and unreadable records. The probe verdicts on AROS are exercised
only by the M1 rehearsal.

## Clean-workspace alpha scenario

[`tests/apkg/test_alpha.py`](../../tests/apkg/test_alpha.py) runs the alpha's
final scenario as one automated host test. It authors an application and its
dependency from TOML, builds and signs them, publishes through a file channel
served over HTTP, discovers through the CLI and the actual PkgManager worker,
adds the source to the root, declines and then installs by name with the
dependency, keeps a personal configuration through an upgrade, refuses a
tampered archive without change, recovers a crash after the commit decision to
the whole new release, rolls back offline with the channel deleted, repairs and
removes while the personal file stays. It runs in `make test-fusion` and under
the sanitizers. It is host evidence: exact-source Linux/macOS CI, both
inherited suites on the final source, the portal and native runtime remain
separate gates.

## Integration audit of source, trust and system additions

The additional source-list, policy, single-root self-update and alpha-scenario
work is retained. The complete inherited working-tree host integration baseline
passed 311 common methods, but targeted cross-feature review reproduced missing
coverage: channel system observations were inserted inside an origin object,
requirements changing during staging could still commit, and an unversioned
legacy/TOML relation was incorrectly encoded as ordered version zero.

The corrections insert plan fields using parsed top-level body token positions,
probe only resolved packages before fetching archives, and recheck requirements
alongside source preconditions at every pre-decision boundary. A durable decision
still recovers offline. Unversioned relations use an empty version and revision
zero. Legacy imports verify authority/target and prohibit added revision floors;
absent identities need an explicit namespace mapping. Omitted upstream registry
recommendations remain unrecorded rather than becoming an asserted empty list.

Eight added regression methods cover opaque/prerelease/zero-revision providers,
TOML floors, channel plan framing and actual CLI/worker installation, pre-fetch
refusal, stage/apply restoration, post-decision completion and legacy identity
scope. The focused 18-method combination passed. The full host integration then
passed 319 common methods, 78 default-product checks, 110 provisional CLI checks,
78 publishing checks and the existing target/application/contract/ARexx gates.
The full sanitizer campaign passed 221 selected methods; actual host CLI/worker
processes are instrumented, while direct ctypes mocks/faults use the ordinary
library. The authoring expectation was updated to the corrected revision floor.
Public symbols, links across 74 documentation files and AROS SDK header/type
checks pass. The published pre-audit checkpoint failed Linux compilation in
[CI](https://github.com/pomaros/apkg-fusion/actions/runs/40476560458) because the
identity module omitted its POSIX feature-test macro; that declaration fix is
included, with identical generated host object bytes and a final focused rebuild.
Exact-source Linux/macOS CI still must qualify the cleaned checkpoint. Native
system probing and UI/runtime behavior require target tests.
The broader release roadmap is not expanded into new alpha blockers. The
SDK builder is explicitly restricted to its AArch64 profile and separates ABI
output directories; shell syntax and unsupported-target refusal are checked.
This is not native build/runtime qualification.

## Native resource probe correction

The SDK's `resources/filesysres.h` declares `FileSysResource` with a Node
prefix, not a Library prefix. Reading `lib_Version` from every resource was
therefore invalid. The resource probe now checks presence only: a resident
resource satisfies minimum zero, while a nonzero minimum stays undetermined.
Absent resources refuse; resident device version checks remain unchanged.

`test_system_native.py` compiles the actual native probe branch against mocked
Exec/DOS services under ASan/UBSan. Its Node-only allocation reproduces a heap
buffer overread with the preceding source and passes with the correction,
including versioned and absent resource/device cases. The AArch64 AROS SDK
header/type check also passes. These checks do not establish native runtime
behavior. Full host integration passes with 320 common methods and the
existing CLI, publication, target/application and contract gates. Exact-source
CI remains pending for this follow-up.

## PkgManager root source controls

The catalogue window now adds/removes a named root source through confirmed
common plans and lists its ordered sources without network access. Profiles and
trust grants remain separate. Native worker dispatch previously omitted source
installation and self-update even though their common bridge handlers existed;
a shared job classification now connects those paths and the new controls.

The worker rehearsal covers CLI-equivalent listing, add/remove, declined and
cancelled plans, allocation failure, duplicate/untrusted sources, offline removal
and balanced public-message ownership. Existing install-by-name and self-update
worker tests also assert native routing selection. All 18 source/self-update
methods pass with the actual worker bridge instrumented by ASan/UBSan. Full
host integration passes 321 common methods, 78 default-product checks, 110
provisional CLI checks, 78 publication checks and the existing contract,
ARexx, target and application gates. AArch64 SDK type checking and links across
74 documentation files pass. Exact-source Linux/macOS CI remains pending;
native UI execution is still deferred to the isolated runtime rehearsal.

## Implemented integration pieces

| Piece | Evidence and limits |
|---|---|
| Explicit target module | Separate CPU/OS/ABI/runtime fields with parse, format and matching. 118 checks pass under strict C99 compilation and ASan/UBSan. Parsing does not certify platform support. |
| Qualified publication | Executable inspection checks the tuple's CPU component; full target identity travels in signed metadata and the channel. |
| Application descriptions | The common library validates `application.json` independently of registration. 315 host checks pass. Native launch and managed bundle movement remain integration work. |
| Provisional CLI baseline | `build/apkg-provisional` keeps the earlier long-option client over the inherited `pkg_lib_*` backend as a regression target; 110 command checks pass. The default `build/apkg` is the common engine and contains no global request backend. |
| Authoring bridge | apkg-pack validation and bundle generation feed native signing. 78 offline checks cover publication, install, dependencies, verification, tampering and refusals. Inputs remain unchanged. |
| Portal qualifications | Review and runtime records apply to exact published content independently of publishing rights. The portal suite has 107 passing host tests. Package and search pages were inspected in a local browser; production deployment remains separate work. |
| Imported arospkg host suite | The complete inherited host suite passes on macOS and Linux in the BASE qualification above, including the pinned LH5 fixture. This is host evidence for the imported components. |
| Imported engine suite | The complete `make test` suite passes on both macOS and Linux; see BASE qualification for the exact source and run. |
| First-install locking | Initially absent roots are locked before resolution and remain locked through the backend flush step. 27 focused checks, including independent competing processes and usage diagnostics, pass; the events suite passes 86 checks. Power-loss durability is separate qualification. |
| Host build recipes | Native macOS and Linux clients build and pass the BASE host suites. An earlier universal macOS build had arm64/x86_64 version smoke checks. Windows and static Linux cross-build recipes remain unqualified. |

These checks ran on a host. The authoring suite's ELF fixture is synthetic and
never executed. ABI/runtime mismatch tests prove selection refusal, not runtime
support. Source identities are recorded in [provenance](provenance.md).

## Bridge restrictions

The bridge accepts application, tool and data drawers, numeric versions, an
absent revision or revision 1, and package dependencies. It stores the unchanged
author TOML as `Package.arospkg.toml` inside the bundle and uses apkg-pack to
generate a declared `application.json`. It adds no review to author metadata.

It refuses system requirements, recommends/conflicts/replaces, distribution
constraints, later revisions, prereleases and shared system destinations.
It also refuses hidden payloads and empty directories that the inherited
container would omit. The [acceptance matrix](acceptance.md) treats these as
integration work, so package semantics cannot disappear silently.

`build/pkg` remains the bridge and portal command during development. The
original pkg API and command syntax have no compatibility commitment in the
common product. Released APKG users require a tested migration path.

## Remaining integration

- Complete PkgManager common-engine wiring beyond the signed-install path, preserving typed browsing, news,
  plans, cancellation, Run, folder opening and settings.
- Port the remaining legacy services/operations to the common contexts and
  typed results; the first common path is independent of global request state.
- Extend immutable plans and precondition validation to the remaining operation
  families, providers, system destinations and trust changes.
- Extend common transactions/recovery to remaining operations; qualify atomic replacement, write ordering
  and durability, including initial roots and interrupted recovery.
- Preserve complete version/revision and package/capability semantics with
  loader-compatible provider selection.
- Finish bundle launching, movement, adoption and payload fidelity, and system
  packages under the common ownership/transaction model.
- Implement migration on both existing APKG distributions, covering settings,
  edits, reasons, relations, launch metadata and rollback history.
- Agree official trust bootstrap, key custody, rotation and recovery policies.
- Adapt consumers to intentional command/API/storage changes. No consumer
  adaptation has been performed in this integration run.
- Qualify each host and AROS CPU/ABI/runtime, including the inherited DOS
  atomic-replacement requirement on existing distributions.
- Complete portal/client/catalogue integration and agree first-release scope;
  retain full distribution installation and upgrade acceptance cases.

Use `make test-fusion` for integration checks and
`dotnet test portal/tests/Portal.Tests` for the portal. Run relevant inherited
and target-specific tests for changed components. Final release evidence
must identify the exact source and builds tested.

The passing portal CI run used .NET SDK 10.0.113, selected within the
repository's 10.0.1xx SDK band by `global.json`'s `latestPatch` policy.

The integration checks comprise 118 target, 315 application-description,
27 first-install, 110 CLI and 78 publication checks. All are covered by the passing BASE matrix; the HTTP fixture test additionally
checks synchronous and threaded responses with reverse DNS unavailable. No operational consumers were
modified and no portal or AROS deployment was performed.

## Follow-up verification register

These checks remain scheduled work; they do not block BASE or contract design.

| Item | Owner batch and evidence to obtain |
|---|---|
| Existing APKG distributions | MAP: AROS One 1.3 is named in the pinned sources; identify the second distribution and exact releases from evidence. |
| DOS and handlers | MAP / PLATFORMS: measure dos.library version/revision and handler capabilities on each distribution; exercise replacement and recovery rather than inferring compatibility from ABI. |
| Self-update state | MAP / JOURNAL / SELF-UPDATE: map both existing journals, including `ENVARC:arospkg/self-update.json`, into one recovery model and test interrupted self-update recovery. |
| Maintainer feedback | ALIGN: collect comments on the selected skeleton, trust administration and release scope when contact is authorized; development continues meanwhile. |
| M1 rehearsal | Operator-authorized after the other fusion workstreams and before portal work. Test candidate binaries/roots separately; identify the exact runtime/build and exercise UI, operations and recovery. No native result is recorded yet. |
| M1 updater | Existing update integration uses `libpkg`. Another owner will migrate it. Preserve the installed library and update/startup process; fusion runtime tests must not replace them. |
| Existing portal | PORTAL-MIGRATE: migration of https://aros-pkg.azurewebsites.net/ is requested after fusion. Inventory real data/configuration, rehearse backup/restore and cutover, verify publishing/consumption and URLs, and retain rollback. No live migration has been attempted. |

Portal migration work keeps credentials and private data outside the public
repository. Existing author signatures, channel trust and qualification evidence
must retain their meaning; migration cannot grant review to unreviewed content.
Per operator direction, porting this portal follows the other fusion workstreams,
including client integration, independent qualification facts and legacy import.
An isolated M1 runtime rehearsal now precedes portal adaptation as well.
