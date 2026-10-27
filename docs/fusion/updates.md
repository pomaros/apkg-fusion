<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Captured candidate comparison

`apkg_channel_compare` compares one authenticated channel row with a captured
common installed snapshot. It uses complete authority/name/target identities,
the common operation version comparator, unsigned packaging revisions and the
exact committed descriptor digest. It fetches no archive, checks no installed
payload and performs no dependency resolution. `plan_readiness` stays UNKNOWN.

| State | Meaning |
|---|---|
| `not-installed` | No committed package has that complete name. Candidate trust and target facts remain separate. |
| `current` | Authority/target and exact descriptor bytes match the installed record, including opaque version spellings. This asserts no live payload health. |
| `newer` / `older` | Common operation ordering establishes version/revision order. Versions order before packaging revisions. |
| `content-conflict` | Different descriptor content has an equal ordered version/revision, or the same exact spelling/revision. No automatic overwrite is implied. |
| `indeterminate` | Version order or required installed facts cannot be established. Different prerelease families and opaque versions retain this state. |
| `other-authority` | The name is already owned by another authority. Trusting both signers does not authorize takeover. |
| `other-target` | The installed and offered targets differ. Candidate agreement with the root target is also reported independently. |

Comparison searches the exact qualified owner/target before considering a
same-name foreign record. Multiple exact identities, or multiple unmatched
owners, refuse with CONFLICT. Unqualified INFO/folder/Run/remove/repair/upgrade
lookups also refuse ambiguous names. Explicit snapshot-index inspection keeps
the original records available for diagnosis; nothing is deduplicated or moved.

Semantically equal spellings retain their exact bytes. A higher packaging
revision follows the operation comparator's behavior; equal revisions with
different content need an explicit resolution. No lexicographic fallback
turns an indeterminate comparison into a newer release.

The snapshot/channel pair must agree on physical root binding, logical root,
policy and target. A policy/root/target change refuses with STALE; a pending
root needs recovery. Replay-floor checks reject old heads and same-sequence
forks. Comparisons describe captured objects, without asserting online freshness.
An earlier observation stays immutable after subsequent root changes.

`apkg_client_channel_refresh` returns independently owned snapshot/channel
objects after validating their compatibility and capturing the accepted floor.
Both outputs clear on failure. Installed snapshots now expose a cached digest
of each exact descriptor; it is computed during the bounded metadata read,
independently of the signature envelope's claimed digest.

The CLI's read-only JSON report is:

```sh
./build/apkg channel-updates ROOT ENDPOINT CHANNEL-ID
```

It retains every candidate and exact selection, observed root/source identity,
installed version/revision/owner/target, candidate trust and target facts,
unasserted qualifications and unperformed plan readiness. Counts/readiness are
not inferred from version order. Applying a selection still uses the shared
dependency-aware plan, source revalidation, confirmation and journal.

The actual PkgManager discovery worker uses that same refresh/comparison path
and copies its results into the owned native catalogue model. The native
catalogue window filters all candidates, updates/blocked candidates, packages
not installed, other targets, untrusted authors, content conflicts and
indeterminate versions. Blocked rows remain inspectable with their installed
owner/target and comparison explanation. Metadata that permits review still
requires a real plan. Older/conflicting/indeterminate/other-owner/other-target
rows cannot start an automatic review through that button.

These views describe their captured installed state; reload to observe later
changes. Legacy OUTDATED aggregation, settings, publication chronology and
broader native/ARexx compatibility remain further work. Host/type/runtime
evidence is tracked separately in [status](status.md).
