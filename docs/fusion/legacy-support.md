<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Legacy support choices and deferred work

CLI-CONTINUITY (batch 3), IMPORT-PROFILES (4) and PRIOR-VERSIONS (5) are deferred
at the operator's request. Their completion no longer gates qualification of
the current candidate on macOS/Linux and M1 or the report before the portal.
This deferral does not qualify migration of an existing installation. Existing
adapters and tests remain intact.

[Status](status.md) owns measured results; the
[plan](../../PLAN.md#closure-batches-before-the-portal) owns the execution sequence.
Sizes below describe relative effort, not calendar commitments.

## Select the support promise

These profiles can be combined: importing installations does not require
reproducing every historical command. Retaining an archive does not
automatically make it usable to restore a version.

| Selected support | Promise and limitation | Additional work | Indicative size |
|---|---|---|---|
| Fresh installations only | Common engine, clients and formats on a fresh root; no automatic import of historical roots or scripts. | Document separate installation and refusals; retain existing adapter tests. | S documentation; qualification remains in active batches |
| Preservation and inspection | Retain originals and their facts, including unknowns; do not implicitly activate settings or history. | Define admitted formats, inventory/export and diagnostics; check copies, permissions, missing data and interruptions against real captures. | M, depending on formats |
| Installation import | Recover applications, personal edits, placement, settings and launch behavior in the common model. | IMPORT-PROFILES below; select old-script support separately. | L |
| Command/script continuity | A named set of historical commands, options, JSON outputs and return codes. | CLI-CONTINUITY for the selected corpus; no implied universal compatibility. | L for the previously planned scope |
| Historical-version import | Restore versions whose identity and bytes are available and admissible. | PRIOR-VERSIONS; explicitly identify impossible restores. | L–XL, with the greatest uncertainty |

## CLI-CONTINUITY — commands and scripts (batch 3)

The [existing profile](legacy-commands.md) covers a native subset of
list/info/verify/install/upgrade/remove/doctor and previews. It does not
guarantee every script from an old distribution.

Select and implement according to the consumers actually supported:

- Map update/search/recent/outdated, grouped upgrades and rollback by name to
  common selections and plans.
- Cover `--index`/`--index-url` catalogues, launch arguments and self-update
  options. Syntax adaptation cannot authenticate an old unsigned catalogue.
- Define JSON fields, missing values, text outputs, return codes and
  confirmation expected by the selected scripts; document required changes.
- Decide whether to exclude test-only injection/report options or provide them
  through test tooling rather than implicitly promising them in the product.

Exit evidence: a versioned script corpus, successful and refused operations on
an imported copy, effects equivalent to common plans, and no silently ignored
arguments. Complete qualification on the environments actually claimed.

## IMPORT-PROFILES — installations, settings and launch (batch 4)

The [existing copy importer](legacy-copy.md) imports a schema-1 profile with
relations and system requirements under explicit approval. Retained settings or
launch bytes are not converted active configuration. The
[mapping](migration.md) describes the data and its uncertainties.

- Pin admitted source versions, schemas and variants; obtain authorized
  original captures with reproducible inventories.
- Complete placement handling: shared paths, icons, owners, collisions,
  modified/unknown files and filesystem attributes.
- Convert settings, ENV/ENVARC associations, roots and sources with their scope.
  Do not infer trust or qualification from a client preference.
- Reconcile launch metadata with `application.json`; validate paths and
  disagreements without executing programs during import.
- Test validation, copying, interruption, recovery and retry; keep the source
  installation usable and refuse unsupported cases.

Exit evidence: source-to-common-state report and verified preservation of the
originals, followed by diagnosis, launch, upgrade, repair and removal on the
copy. Inspect files, personal data and journal; add native tests needed for DOS
attributes.

## PRIOR-VERSIONS — history and restoration (batch 5)

Rollback of versions created by the common engine remains in active tests.
Only conversion of historical state is deferred here.

- Inventory earlier registries, archives, references and self-update journals:
  available, missing, unresolved and unverified are distinct states.
- Define identity, version/revision, target, provenance, admissibility and
  ownership for each imported version. Retained bytes gain no retrospective
  signature or authorization.
- Construct entries usable by common plans and the common journal; separately
  preserve evidence that cannot be converted.
- Define retention/cleanup and handling of unfinished historical journals,
  without running two recovery models over the same mutation.

Exit evidence: offline restoration of versions declared available, explained
refusal of other cases, interruption/recovery, and inspection of personal edits
and recorded state. A missing archive prevents promising its restoration.

## Information needed before resuming

The second distribution, deployed APKG versions, `dos.library` versions,
filesystem handlers and real scripts are not established. The mapping identifies
AROS One 1.3 from upstream documents; that is neither a captured installation nor
qualification of the distribution.

Before making a support promise, name its profile, versions and corpus, collect
these inputs and define refusal/fallback conditions. Maintainer agreement and
distribution qualification remain conditions of a joint release; host tests do
not establish them.

## Keep these responsibilities separate

Migration between versions of the APKG tool itself—executable, library,
persistent formats, clients, backup and rollback—has a separate review item in
[TODO](../../TODO.md). Application import does not prove tool migration. Another
owner will migrate the `libpkg` consumer that updates the M1. The portal remains
after the requested stop boundary.
