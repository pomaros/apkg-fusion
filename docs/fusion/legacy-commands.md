<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Legacy command rehearsal

Completion of historical support is deferred from the current candidate.
See [support choices and remaining work](legacy-support.md). Existing tested
adapters remain available without a complete migration/compatibility claim.

`apkg legacy ...` and the installed `apkg-legacy` alias accept a defined
subset of released APKG command spelling over the common engine. The adapter
is native C and also builds for the AROS SDK; host execution qualifies the
rehearsal below, while native runtime and distribution/script rollout remain
separate gates. It uses no imported mutation engine and never switches a live
legacy root to common storage. Prepare an original-preserving
[imported copy](legacy-copy.md) first.

The pinned source is [the imported CLI](../../components/arospkg/src/pkg/main.c),
whose revision is recorded in [provenance](provenance.md). This is a versioned
compatibility profile, not a claim that all old scripts or output fields work.
Unsupported commands/options refuse with exit 5 before starting an operation.

## Supported profile

```
apkg-legacy --root COPY list [--json]
apkg-legacy --root COPY info NAME [--json]
apkg-legacy --root COPY verify NAME
apkg-legacy --root COPY install NAME --yes
apkg-legacy --root COPY upgrade NAME --yes
apkg-legacy --root COPY remove NAME --yes
apkg-legacy --root COPY doctor [--retry]
```

`--root` may precede or follow a command. The original `SYS:Packages` default
is retained; host rehearsals supply the copy explicitly. `--plain` and
`--color=never` select the adapter's ordinary plain output. Exit 0 means the
command completed, 5 means arguments/unsupported behavior or an unapproved
plan, and 10 means a common-engine operation failed. There is no trailing common
`status=...` record mixed into legacy JSON.

Install/upgrade resolve names against the root's ordered signed
[source list](sources.md#root-source-list-controls). Configure it explicitly
with common source controls before the script rehearsal. Upgrade/remove require
an installed name. All mutations use common plan approval and journal/recovery.
Without `--yes`, the adapter prints the actual held plan and returns 5 with its
`--confirm DIGEST` retry argument. A changed plan cannot reuse the old approval.
`--yes` approves the plan the same invocation displays and executes.

`remove --dry-run [--json]` previews without managed-state changes. Install and
upgrade previews require `--dry-run --fetch [--json]`: their complete common
plans need verified payload bytes, and `--fetch` explicitly authorizes archive
preparation/retention. A source preview without `--fetch` refuses before transfer.
JSON preview is the canonical common plan, with its own versioned format;
old consumers of the old dry-run JSON must adapt. An already-current package
returns an explicit compatibility result with `outcome: unchanged`, without
fabricating a plan. A successful dry run returns 0.
`doctor --retry` recovers only the common journal. Recovery can require operator
attention; there is no force/unlock translation that discards unresolved state.

List prints sorted installed names, one per line, or `nothing installed`.
JSON list retains `rows` and `hidden`, with id/version/revision, target/kind and
installed/ours fields. `ours` means the explicit root target matches, not runtime
qualification. Revision numbers and names are complete rather than truncated.
The extra `compatibility` identifier is `apkg-legacy-compat-1`.
Info emits one current installed metadata object with name/dir/schema aliases.
It describes the current common record, so upgrading a migrated package cannot
keep presenting the old retained registry version as current.

The common record does not establish old archive size: size is -1.
Summary/category come from [signed descriptive metadata](presentation.md) when
recorded in the current descriptor. Otherwise they are null; `unrecorded` names
each absent field. Recorded empty text stays empty. These fields describe the
current signed artifact, not retrospective authentication of an old registry.
Signature scope is the descriptor; trust is a separate current captured-policy
fact. Neither authenticates the original legacy installation retrospectively.
Raw old registry presentation, launch and history remain retained evidence;
only descriptive text in the current signed descriptor becomes current INFO.
Scripts reading the other retained fields
fields require further adapters. The original records remain accessible through
the private import evidence, not rewritten by the compatibility command.

Verify uses the shared [installed diagnostic](inspection.md): success means the
report completed, while changed/missing/unknown paths remain explicit facts.
No repair is implied. Read-only commands refuse unsettled roots except doctor,
which can diagnose the need for common recovery.

## Explicit remaining work

This profile refuses update/search/recent/outdated, raw legacy `--index` and
`--index-url`, group upgrades, rollback-by-name, launch argument forwarding,
self-update switches and test-only injection/report options. Their contracts
remain acceptance obligations; refusal is not completion. The old unsigned
catalogue is not silently treated as an authenticated common channel.
Production URL continuity, real distribution scripts, settings/launch/history
conversion, native behavior and broader legacy JSON compatibility still need
qualification. Existing endpoints are not retired by this workspace.

The executable rehearsal imports the pinned schema-1 fixture into a copy, then
lists and reads current metadata, adds a signed local source, previews and
confirms an upgrade, verifies edits, installs/removes another package and removes
the migrated package while preserving personal data. It checks source inventory
and retained registry bytes throughout. Refused arguments and direct mutation
of an unmigrated root leave those originals unchanged. The installed alias is
also exercised from a temporary installation prefix.
