<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Implementation status

This is a development integration workspace. It does not replace released APKG
installations. The checkout starts from pkg with an arospkg import. The
[architecture](architecture.md#implementation-skeleton) selects arospkg’s
context-based core as the skeleton; porting the services remains implementation
work, independently of the provisional source layout.

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
This is a design deliverable; no common database writer or importer exists yet.

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
can run. This completes the initial CONTRACT design, not the common engine.

`make test-contract` runs an executable inspection/UI event example against a
labelled in-memory test double, compiles initialization/install client flows,
checks 54 inherited version/target assertions and checks golden canonical
bytes/signature transcripts plus 19 malformed-input/integer rejection cases.
The same checks pass with AddressSanitizer and UBSan. Documentation link check:
61 files, zero broken links. No production API implementation, actual signature
verification, AROS worker execution or filesystem behavior is established by
these examples. The fake is outside all production source/install lists.

Focused design review resolved explicit first-root initialization/trust,
unknown snapshot identity, post-commit cancellation, approval lifetime/reuse,
callback ownership and allocation failure after mutation. No independent
maintainer approval or full engine/platform CI result is claimed for this task.
The contract check is included in `test-fusion` for subsequent CI runs.

Next batch: CONTEXT. Implement only explicit context open/close and read-only
root snapshots, with independent roots, absent-root no-write behavior, snapshot
consistency and pending-recovery reporting. Legacy/unsupported state remains
inspectable; planning, initialization, recovery and signatures follow in their
own batches. The provisional production backend is still unchanged.

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

- Connect PkgManager to the common engine, preserving typed browsing, news,
  plans, cancellation, Run, folder opening and settings.
- Replace global operation state with explicit contexts and shared typed
  results for clients and embedded consumers.
- Implement immutable plans, complete precondition validation and execution
  of exactly the approved content, providers, destinations and trust changes.
- Unify transactions and recovery; qualify atomic replacement, write ordering
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
