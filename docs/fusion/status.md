<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Implementation status

This is a development integration workspace. It does not replace released APKG
installations. The checkout starts from pkg with an arospkg import. The
[architecture](architecture.md#implementation-skeleton) selects arospkg’s
context-based core as the skeleton. The initial common installation path is
implemented below; remaining service/operation integration is listed separately.

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

## Implemented integration pieces

| Piece | Evidence and limits |
|---|---|
| Explicit target module | Separate CPU/OS/ABI/runtime fields with parse, format and matching. 118 checks pass under strict C99 compilation and ASan/UBSan. Parsing does not certify platform support. |
| Qualified publication | Executable inspection checks the tuple's CPU component; full target identity travels in signed metadata and the channel. |
| Application descriptions | The common library validates `application.json` independently of registration. 315 host checks pass. Native launch and managed bundle movement remain integration work. |
| Native development CLI | `build/apkg` offers long options, explicit targets and `bundle-check`; 110 command checks pass. Its inherited engine backend still contains global request state. |
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
| Existing portal | PORTAL-MIGRATE: migration of https://aros-pkg.azurewebsites.net/ is requested after fusion. Inventory real data/configuration, rehearse backup/restore and cutover, verify publishing/consumption and URLs, and retain rollback. No live migration has been attempted. |

Portal migration work keeps credentials and private data outside the public
repository. Existing author signatures, channel trust and qualification evidence
must retain their meaning; migration cannot grant review to unreviewed content.
