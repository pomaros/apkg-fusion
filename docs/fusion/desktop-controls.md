<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Desktop controls and automation audit

This inventory defines the non-demo fusion client's exposed surface and the
evidence required to qualify it. The window lives in
[main.c](../../components/arospkg/src/pkgmanager/main.c), the worker bridge in
[common_worker.c](../../components/arospkg/src/pkgmanager/common_worker.c), and
typed model/query adapters in
[common_browser.c](../../components/arospkg/src/pkgmanager/common_browser.c).
Implementation results belong to [status](status.md); a host helper or SDK
type check is not a native Zune/ARexx interaction result.

## Window surface

| Control group | Common behavior | Host regression evidence |
|---|---|---|
| Main search, category, state and details | Combined complete installed/candidate identities, dynamic categories, exact selection preservation, signed publication dates and previous/current observation comparison. | [Library](../../tests/apkg/test_library_browser.py), [recent](../../tests/apkg/test_recent.py), [news](../../tests/apkg/test_news.py), [inspection](../../tests/apkg/test_inspection.py) |
| Refresh signed catalogue | Bounded metadata discovery; selected source profile captures the two authenticated observations. Explicit no-baseline/error/zero-change outcomes. | [Channel clients](../../tests/apkg/test_channel_clients.py), [profiles](../../tests/apkg/test_source_config.py), news |
| Catalogue window search, filters, selection, recompare and close | Typed provenance/trust/target/comparison/qualification facts; offline revalidation; cancellation and parent-model lifetime guards. | [Browser](../../tests/apkg/test_browser.py), [summary](../../tests/apkg/test_summary.py), news |
| Source endpoint/id/profile, Save and Restore | Explicit client settings separate from root state. Save retains a matching pair; Restore reads without network; source changes clear baseline. | Profiles, news; [source contract](sources.md) |
| Add, Remove and List root sources | Ordered root-owned source list; mutations are approved journaled plans, distinct from profile settings. | [Sources](../../tests/apkg/test_sources.py) |
| Install selected candidate, Upgrade | Exact signed selection or ordered root-source resolution, dependency plans and confirmation. | Channel clients, sources, [updates](../../tests/apkg/test_updates.py) |
| Remove, Verify, Rollback | Current installed identity, independent live-file diagnostics, retained common history, ownership/configuration preservation. | [Verify](../../tests/apkg/test_verify.py), [rollback](../../tests/apkg/test_rollback_last.py), [operations](../../tests/apkg/test_operations.py) |
| Run, Open folder | Resolve the managed application drawer and enforce launch/path/target rules; opening a folder does not authorize executing it. | [Bundles](../../tests/apkg/test_bundle.py), [authoring](../../tests/apkg/test_author.py), [installed browser](../../tests/apkg/test_installed_browser.py) |
| Review operation selector | Local signed install/upgrade, remove, repair, retained rollback, local-pool resolution, recovery, exact channel selection/acceptance, bundle/set adoption, trust-policy replacement and undo latest package transaction use the common client action enum. | [Client](../../tests/apkg/test_client.py), [adoption](../../tests/apkg/test_adopt.py), [policy](../../tests/apkg/test_policy.py), rollback |
| Inspect signed claim | Common signature/role/subject observations; reviewer/runtime facts do not become author grants. | [Attestation](../../tests/apkg/test_attestation.py), [channel observations](../../tests/apkg/test_channel_attestation.py) |
| About: check client version and self-update | Signed managed-client plan in the common journal; active other windows and legacy journals/copies are explicit limits. | [Self-update](../../tests/apkg/test_self_update.py), [contract](journal.md#single-root-self-update) |
| Proceed, Decline, Cancel and Quit | Held-plan confirmation, phase-aware cancellation, accurate final outcome; worker lifetime ends before program teardown. | Client, channel clients, [actual dispatcher](../../tests/apkg/test_pm_dispatch.c) |

The settings scope here is root selection at startup plus explicit source
settings, observations and the root's source list. Historical ENV/ENVARC
preferences, launch associations and imported client settings are in the
deferred [legacy support profiles](legacy-support.md). No general preference
editor or unimplemented settings migration is implied by a Save source button.
Demo-store purchases, pictures and test injection controls are outside this
non-demo audit; they do not provide package mutation authority.

## ARexx surface

The fusion interface advertises version 2. The
[parser](../../components/arospkg/src/pkgmanager/rexxcmd.c) and window dispatcher
expose the following commands; no query opens the inherited mutation backend.

| Commands | Contract and evidence |
|---|---|
| VERSION, HELP, LASTERROR | Interface/binary identity, advertised commands and per-client last error; inherited parser/result corpus also compiles in fusion mode. |
| LIST, LIST INSTALLED | Revalidated catalogue names or current installed rows; MATCH and result bounds are explicit. [Catalogue tests](../../tests/apkg/test_rexx_catalogue.py), installed browser. |
| INFO | Installed metadata first, otherwise one unambiguous revalidated catalogue build; complete declared facts, independent trust/target/qualification facts and no invented installed state. [Inspection](inspection.md). |
| REQUIREMENTS | Probe through the common package/system query with explicit unavailable/unknown results. [Tests](../../tests/apkg/test_requirements_query.py). |
| SHOW | Fresh offline combined list, installed precedence, unique catalogue fallback, BUSY/CONFLICT/NOTFOUND errors. Library tests; native list selection requires target interaction. |
| OUTDATED [SHOW] [FIELD] | Common current-root comparison and summary; SHOW respects model lifetime and brings the catalogue forward. [Tests](../../tests/apkg/test_summary.py). |
| UPDATE, INSTALL, UPGRADE, REMOVE, ROLLBACK, RUN, VERIFY | Same worker/job classification and common operation flow as the window; no script bypass of plan confirmation. Actual dispatcher and relevant operation tests above. |
| JOB, WAIT, CANCEL, QUIT | Asynchronous completion, progress, cancellation and teardown; errors and outcomes stay distinct. Fusion parser/job corpus and actual dispatcher tests. |

Old CLI spelling/output continuity is a separate deferred lot. Its deferral
does not remove these commands from the current desktop contract.

## Native qualification boundary

The completed candidate must exercise the visible controls, ARexx queries and
asynchronous commands on the M1 after host qualification. Inspect install →
verify/use → remove, post-removal registration/files, personal/configuration
preservation, pending transactions and worker/process exit. Exercise selection
with candidate-only rows, conflicting names and active filters, plus busy,
confirmation and close paths. Keep the installed M1 updater migration separate;
test client self-update only inside the approved isolated test root.

Repeat confirmation windows at the same size: decline an upgrade, reopen and
approve it, then request rollback and removal. Each opening must display its
own nonempty plan and exact digest before Proceed can be used. On Zune, changing
Floattext while its window is closed can clear its layout without rebuilding
it when the unchanged-size window reopens; host worker tests do not exercise
this rendering lifecycle.

After VERIFY completes, JOB and WAIT (including FIELD message) must retain the
verification counts/report independently of the worker message and the window's
report buffer. Exercise a subsequent job or model refresh before reading the
completed job again; successful verification must not return an empty error
summary in place of its report.
