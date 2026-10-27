<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Non-portal closure checklist

The operator's order is: finish the fusion outside the portal, qualify it on
macOS/Linux, test the resulting candidate on the M1, then stop and deliver a
detailed comparison with original APKG. Portal adaptation requires a later
operator instruction. Partial portal work and the
earlier isolated RAM component rehearsal do not close these requirements.
The [plan](../../PLAN.md#immediate-work-sequence) owns the scope and order;
[status](status.md) records evaluated results. Keep implemented capabilities,
incomplete integrations and broader release qualification distinct. Refusal of
an unsupported feature is safe behavior, not proof that feature is finished.

| Area | Evidence to inspect | Closure requirement before advancing |
|---|---|---|
| Common engine and default clients | [Client API](../../include/apkg_client.h), [worker](../../components/arospkg/src/pkgmanager/common_worker.c), [alpha scenario](../../tests/apkg/test_alpha.py), symbol and dispatcher checks | One engine and approved plans cover the implemented operation surface; CLI/worker results and failures agree on current source. |
| Real application workflow | [Authoring tests](../../tests/apkg/test_author.py), [managed Run](../../src/apkg/client_ops.c), [bundle inspection](../../src/apkg/bundle.c) | A real authored, installed application resolves its declared application drawer through both clients. Reject ambiguous or escaping locations; host preparation is distinct from execution on AROS. |
| PkgManager controls and automation | [Window](../../components/arospkg/src/pkgmanager/main.c), [combined library](desktop-library.md), [ARexx](../../components/arospkg/src/pkgmanager/rexxcmd.c), source-profile tests | Audit every exposed control and promised command. Source settings are persisted; Upgrade uses root sources; Rollback uses the common transaction plan. The main list combines installed and signed candidate rows with category, search and All/Installed/Not installed/Updates filters. Recent/last-refresh still needs a common model. Close that gap and the remaining automation/settings work with explicit evidence, then test interactions on M1. |
| Legacy copy and continuity | [Mapping](migration.md), [copy adapter](../../tools/apkg_migrate.py), [compatibility limits](legacy-commands.md), migration tests | Check the declared import profile and pinned fixture against actual source, receipt, committed state and subsequent operations. Keep originals usable. Distinguish retained launch/settings/history evidence from active conversion; complete assigned conversion/compatibility work rather than relabeling it as preservation. |
| Final host qualification | [Workflow](../../.github/workflows/host.yml), common/inherited suites and clean-workspace scenario | Current-source integration, memory checks and both inherited suites pass on Linux/macOS. Exercise installation, verification/use and removal, then inspect registration, managed-file removal, personal/configuration preservation and absence of a pending transaction. Old checkpoints and parser/type checks cannot prove a changed candidate. |
| M1 candidate test | [Rehearsal](../../tests/aros/README.md), [prior component evidence](evidence/m1-ram/README.md) | Test the completed candidate through installation, verification/use and removal, followed by database/file checks and independent worker/process-exit checks. Record exact binaries, operation results and limits. Keep the installed `libpkg` updater and its migration separate. No OS source changes, live distribution migration or power-loss claim follows. |

The audit must not silently add every broader release obligation as an alpha
blocker, or silently defer explicit non-portal deliverables because another gate
passes. Unknown distribution samples and maintainer agreement remain release
dependencies as recorded in the plan. Remaining implementation findings belong
in status with an action and acceptance test.
