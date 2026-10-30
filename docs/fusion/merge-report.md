<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# APKG fusion report

This report covers the fusion work outside the portal: the common engine and
clients, host qualification and the application lifecycle on the M1. Measured
results and counts belong in [status](status.md), and obligations in the
[closure checklist](non-portal-checklist.md). These results are distinct from
general release qualification. Work stops before the portal, which requires a
later instruction.

A subsequent [selective arospkg 0.7.2 overlay](upstream-0.7.2.md) brings link UI
and authoring fixes plus legacy self-update corrections into the workspace.
Its host/cross-build checks are separate from this report's frozen M1 evidence.
Joint decisions remain open in the [maintainer handoff](maintainer-agreement.md).

## What is retained and what changes

The fusion adopts APKG's model: an explicit context per root, plans, a recovery
journal, a CLI and PkgManager with its DOS/ARexx worker. APKG already had these
capabilities, dependencies, revisions, prereleases, recent-publication views and
review/test indicators. They are inherited capabilities, not inventions of the
merge. The [imported README](../../components/arospkg/README.md) describes that
starting point.

The default product engine is a common implementation in `src/apkg`, built
around this model and pkg's portable services. It does not alternate between
two package managers according to the operation. The original engines remain
behavioral references and regression suites. See the
[architecture](architecture.md) for implementation details and limits.

| Area | Change relative to APKG | Evidence and limitation |
|---|---|---|
| Engine and clients | The CLI and PkgManager worker call one common API with typed results and shared plans. The provisional backend with global request state is excluded from the default product. | Host symbol, routing and equivalence checks; GUI/ARexx mutations and independent CLI inspection on M1. The exhaustive native interaction matrix remains a separate release qualification. |
| Host use | The common CLI, library, packaging and publishing tools build and operate on macOS/Linux. | Real host operations are tested; this does not mean AROS applications execute on macOS or that Windows is qualified. |
| Package identity | Authority, name, version, revision, target and exact content are separate. Two authors can publish the same name without merging their identities. | Ambiguous choices refuse; interfaces retain complete values. |
| Targets | CPU, OS, ABI and runtime travel together in the signed description. | Metadata compatibility is not an execution test. |
| Signatures and trust | Descriptors, catalogues and attestations use separate signatures; author, channel, reviewer and tester roles are independent. | Each root has an explicit policy. A valid signature does not imply automatic trust, review or hardware validation. |
| Open publication | An author can produce a signed file or HTTP channel, consumed directly without a central catalogue or prior review. | Packaging/publishing workflows are tested; full portal adaptation is outside this sequence. |
| Authoring source | TOML and supported metadata feed the common descriptor, bundle, archive and signatures. | Inputs remain intact; unsupported semantics refuse explicitly. |
| Descriptive information | Summary, category, licence and source repository flow from TOML through the signed descriptor and catalogue to INFO. Lists retain these texts; main-list search and categories use complete values. | An absent field is distinct from an empty string. |
| Recent publications | The channel signs first-publication dates for the application and target build and retains them across versions. CLI, offline reading and Recently added share one selector. Last refresh compares two authenticated observations retained in the profile, distinguishing first observation, no changes, new builds and version/content changes. | Dates are channel-publisher claims, not proof of clock accuracy, online freshness, review or execution. A historical observation cannot become an authorized installation source or lower the replay floor. |
| Transport | Archives and objects transfer in bounded chunks, are hashed and bind to the exact selection before installation. | Corruption, truncation, altered cache and interruption cases are tested; AROS network transport remains to be qualified. |
| Multiple sources | Each root owns an ordered source list; selection and dependencies use the common resolver. | No silent choice between authorities sharing a name, and no silent omission of an unavailable source. |
| Replay protection | Explicit acceptance records the catalogue sequence and digest through the root journal. | Package rollback does not lower this floor; online freshness is a separate question. |
| Confirmation | The approved plan identifies exact content, root, policy and observed sources. Changed preconditions invalidate approval. | Confirmation does not perform a different resolution. |
| Independent review and testing | Signed attestations apply to an exact descriptor and their declared scope. Observations can be retained offline with provenance. | Changed packages do not inherit qualification; authors cannot grant themselves a reviewer role. |
| Configuration and ownership | Install, adoption, upgrade, repair and removal share rules for delivered files, personal edits and unowned paths. | Preservation and ownership-refusal tests pass; native extended-metadata qualification remains open. |
| Rollback and repair | The engine retains verified content and uses the same journal for offline operations. The Rollback shortcut selects the latest applicable package transaction and confirms all its participants. | It undoes a whole transaction, not an arbitrary earlier change to the selected package alone. Retained historical APKG evidence does not automatically become usable common rollback history. |
| Inspection | Clients separately expose declared content, signature, trust, target, installed state and live-file verification. | Unknown facts are not converted into verified values. |
| PkgManager/ARexx | Signed catalogues, complete values, exact selections, progress counters and confirmation use common adapters. The main list combines installed and candidate rows with categories, primary filters, Recently added and Last refresh. Upgrade uses root sources; Rollback and its ARexx command use the common plan. | On M1: queries, busy/ambiguity refusal, declined and approved upgrade, confirmed rollback/removal, observations 1 → 2 and 2 → 2, and offline restoration. A blank reopened confirmation was fixed and retested at the same size. The test client sends ARexx messages directly; the image has no RX interpreter. Broader legacy support and exhaustive native interactions remain separate. |
| Run and bundles | Standalone bundle inspection needs no registration. Managed Run resolves the declared application drawer, including a nested drawer. | The real Calculator launches from the managed package through both CLI and PkgManager/ARexx, with interaction observed; a moved unregistered bundle also launches on M1. Multiple ambiguous choices refuse. |
| Self-update | A client managed in one root uses the common plan and journal. | Copies spread across volumes require further work. The M1's installed updater is not migrated here. |
| Migration | Copy preparation, inventories, explicit association with signed artifacts, exact approval, grouped import and recovery. | Originals remain usable. A pinned profile/fixture is tested; retaining settings, launch or history bytes does not establish active conversion or migration of a real distribution. |

## Changes integrations must account for

Common storage under `.apkg` and the `apkg_*` API are not a transparent binary
replacement for the old library and registry. The migration adapter prepares a
copy; it does not silently convert a live installation. Another owner will
migrate the `libpkg` consumer that updates the M1; that work remains separate
from these trials.

The [historical command profile](legacy-commands.md) describes the commands,
outputs and return codes actually adapted. It does not promise compatibility
with every old script. Refused variants, settings/launch/history conversion and
catalogue continuity remain explicit migration obligations.

Trust and sources require explicit configuration. Importing an installation or
adding a source neither retroactively authenticates old files nor adds keys
without the operator's knowledge.

## Validation and closure

Host tests exercise real operations on temporary roots, packages, signatures,
channels, refusals and recovery. They are more than compilation checks. Process
failure simulation and sanitizers do not establish power-loss durability.

An [earlier isolated M1 profile](evidence/m1-ram/README.md) exercised CLI and GUI
operations in RAM, finding file-identity, cache-parent creation and DOS-permission
observation issues. It retains its component/runtime limits and does not
validate subsequent changes or the whole fusion. The
[final M1 lifecycle evidence](evidence/m1-final/README.md) adds application
observations and verified outputs for the corrected candidate.

The product candidate is `f73eec77d3c0aba17fe5e7b3b65cc22c3f918c53`. Its
[exact-source Linux/macOS CI](https://github.com/pomaros/apkg-fusion/actions/runs/40562000506)
passes all four jobs: fusion and portal baseline regressions on both hosts.
Binary hashes, results and limits are recorded in the M1 verdicts and status.
The CLI is identical to the preceding host candidate; native client changes fix
reopened confirmation layout and retain verification reports in ARexx jobs.
Two verifications with different results establish that the first job keeps its
bounded report prefix after the window's report is replaced. With long package
identities, that 479-byte ARexx message can omit counts without a truncation
indicator; the full report remains available in the window and CLI. Improving
this summary is tracked in [TODO](../../TODO.md). These records imply no portal
deployment, maintainer agreement, real-distribution migration or joint release.

The requested cycle executed installation, verification/use and removal.
Independent post-removal checks establish an empty registry, removal of
unmodified managed files, preservation of personal data and edited configuration,
and no pending transaction, preparation or surviving worker. The follow-up also
preserves an icon whose manual restoration changed permissions; its explicit
cleanup as a test residue is recorded separately from the APKG transaction.
Retained history/cache content follows its own policy and is not treated as a
failed uninstall.

Completion of broader legacy compatibility—commands/scripts, installation
conversion and historical versions—is deferred at the operator's request.
[Legacy support choices](legacy-support.md) describe possible scopes and the
work each requires. The current candidate does not promise complete migration;
existing adapters and tests remain intact.

The M1 environment is a retained RAM reference under a hypervisor, with CRT
differences and failed secondary Exec startup. Tested applications run on one
active AROS core. This does not qualify AROS SMP or a new OS runtime. AROS
HTTP/TLS, extended metadata and other handlers, interrupted native recovery and
power loss, native self-update and multi-volume copies, and the exhaustive
desktop interaction matrix remain separate release qualifications or work.
Migration between versions of APKG itself and the proposed diagnostic command
are recorded in [TODO](../../TODO.md).
