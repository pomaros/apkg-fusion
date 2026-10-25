<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Implementation status

This is a development integration workspace. It does not replace released APKG
installations. The checkout starts from pkg with an arospkg import; the
[architecture](architecture.md) is context-based and is implemented
independently of that provisional layout.

## Implemented integration pieces

| Piece | Evidence and limits |
|---|---|
| Explicit target module | Separate CPU/OS/ABI/runtime fields with parse, format and matching. 118 checks pass under strict C99 compilation and ASan/UBSan. Parsing does not certify platform support. |
| Qualified publication | Executable inspection checks the tuple's CPU component; full target identity travels in signed metadata and the channel. |
| Application descriptions | The common library validates `application.json` independently of registration. 315 host checks pass. Native launch and managed bundle movement remain integration work. |
| Native development CLI | `build/apkg` offers long options, explicit targets and `bundle-check`; 110 command checks pass. Its inherited engine backend still contains global request state. |
| Authoring bridge | apkg-pack validation and bundle generation feed native signing. 78 offline checks cover publication, install, dependencies, verification, tampering and refusals. Inputs remain unchanged. |
| Portal qualifications | Review and runtime records apply to exact published content independently of publishing rights. The portal suite has 107 passing host tests. Package and search pages were inspected in a local browser; production deployment remains separate work. |
| Imported arospkg host suite | The complete inherited host suite passes after restoring its missing pinned LH5 fixture. This is host evidence for the imported components. |
| Imported engine suite | `make test` passed before the first-install change. The subsequent full run exposed diagnostic regressions; these were corrected and focused e2e (272), events (86), first-install (27) and fastarchive (52) checks passed. Full final-revision validation is delegated to CI; it is not claimed locally. |
| First-install locking | Initially absent roots are locked before resolution and remain locked through the backend flush step. 27 focused checks, including independent competing processes and usage diagnostics, pass; the events suite passes 86 checks. Power-loss durability is separate qualification. |
| Host build recipes | The macOS universal recipe built arm64 and x86_64 slices before the final diagnostic correction; version smoke checks ran natively and under Rosetta. Windows/Linux recipes lack local toolchain qualification. |

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

The portal suite was run with installed .NET SDK 10.0.105 from a working
directory outside the portal SDK pin. The repository requests 10.0.112,
which was unavailable locally; the pinned-SDK CI run is separate evidence.

The integration checks comprise 118 target, 315 application-description,
27 first-install, 110 CLI and 78 publication checks. The last three were
rerun after the final diagnostic correction. No operational consumers were
modified and no portal or AROS deployment was performed.
