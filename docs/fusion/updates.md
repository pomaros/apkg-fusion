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
changes. Package aggregation and OUTDATED are described below. Settings,
publication chronology and broader native/ARexx compatibility remain further
work. Host/type/runtime evidence is tracked separately in [status](status.md).

## Package summary and OUTDATED

`apkg_channel_summarize` aggregates by qualified installed authority/name/target.
Multiple newer builds of one package count once in `newer`, while
`newercandidates` counts each build. It retains independently owned entries,
installed versions/revisions and candidate counts, after snapshot/channel close.
Exact duplicate installed identities refuse with CONFLICT.

`metadataeligible` counts packages with at least one newer candidate whose
descriptor signature, author grant and root target permit metadata-level review.
`blocked` counts newer packages with no such candidate; `blockedcandidates`
counts those rejected newer builds. Dependencies, ownership effects, live files
and runtime requirements still need a plan. `ready` remains `unknown`, and
`planreadiness` is `not-checked`. Conflicting and indeterminate candidates have
separate counters; zero known newer packages does not erase those cases.

`notlisted` means the qualified installed identity has no matching offer in
this observed channel. It asserts no global withdrawal. Publication time,
online freshness and stale-source status stay `unknown`; no dates or freshness
are inferred from sequence numbers or signatures.

```sh
./build/apkg outdated ROOT ENDPOINT CHANNEL-ID
./build/apkg outdated ROOT ENDPOINT CHANNEL-ID newer
./build/apkg outdated ROOT ENDPOINT CHANNEL-ID qualifiedpackages
```

`apkg_outdated_fields` lists scalar, identity and package-list fields. Formatting
accepts the library view or its unchanged owned deep copy, retaining unknown
readiness/freshness. Unsupported known readiness/freshness flags refuse.
`packages` retains bare names only when unambiguous in the installed namespace;
otherwise that field/full answer refuses. Scalar fields and `qualifiedpackages`
remain available without selecting an arbitrary owner.

The native discovery worker copies the complete summary view, entry array and
all text into its one `AllocVec` catalogue. The catalogue displays observed
generation, package counts, metadata eligibility, conflicts and uncertainty.
Fusion ARexx `OUTDATED [SHOW] [FIELD name]` shares that formatter. `SHOW` opens
the signed catalogue's updates/blocked-candidates view. Results over 65,535
bytes refuse with TOOLONG; individual fields remain available.

ARexx `UPDATE` and the main Refresh button use the signed-discovery worker with
the endpoint/channel controls; they do not invoke the legacy index writer.
Source settings/persistence still need their common integration. The native
model retains an exact copy of the channel object and its signature in the same
allocation as its displayed rows. `apkg_channel_observation` exposes these two
borrowed byte spans for copying; their validity ends with the channel owner.
They retain no archive-transfer origin and authorize no installation.

Cached OUTDATED performs no network requests. It re-reads the signed observation
against the current read-only root context, verifying signatures, current
channel/author grants, target agreement and the durable replay floor, then
summarizes against a fresh installed snapshot. Package changes therefore
recompute counts without downloading the catalogue. Withdrawn channel trust,
older observations, same-sequence forks and pending roots refuse explicitly.
Author withdrawal or target changes affect metadata eligibility separately from
whether a candidate version is newer. Online freshness remains unknown.

Without SHOW, OUTDATED returns newly captured summary fields and leaves the
currently displayed observation unchanged. `OUTDATED SHOW` requests an owned
replacement through the same offline model builder, formats its summary, and
only then replaces the UI model. Returned counts, details, comparison filters,
author/target facts and generation therefore describe the same snapshot. A
failed query (including an invalid field or result overflow) frees the tentative
replacement and preserves the original. SHOW refuses while an operation owns
the model; scalar queries remain read-only.

The catalogue's Recompare offline button runs this rebuild in the existing
worker. The worker borrows the UI's immutable AllocVec until PM_DONE and returns
a new independent AllocVec; no library-owned handles cross worker exit. The UI
clears borrowed list/detail references before replacing a successful model.
Refusal or cancellation retains the previous model under its observed
generation. Cancellation is checked before and after the bounded metadata
rebuild; it does not interrupt an individual signature/parser call. The signed
source identity/snapshot and exact package selections remain unchanged, with
online freshness unknown and no archive transfers or managed-state writes.
Persistent source settings and a durable signed-observation cache remain open.
