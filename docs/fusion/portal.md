<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common portal contracts

Portal adaptation resumed on 2026-10-30. It proceeds as a provisional
integration while [joint maintainer decisions](maintainer-agreement.md) remain
open. These checkpoints do not imply design agreement or authorize a live
cutover.

The portal serves the [common channel layout](channels.md#atomic-local-publication)
alongside the retained legacy channel files. A common channel has an explicit
`current` reference and immutable `snapshots/<sha256>/channel.json` and
`channel.signature.json` members. Archive URLs retain the common layout.
`current` is revalidated on every read; snapshot responses are immutable and
support HTTP ranges. Common publication uses the explicit authenticated commit
procedure below; the legacy commit never publishes common snapshot metadata.

## Authenticated publication

Publishers keep both signing keys on their own machines. They build and sign
archives and a complete common snapshot with the common authoring/file producer,
then use the existing authenticated `/<channel>/_push/plan` and `files/<path>`
endpoints. Snapshot members are bounded metadata; archives retain the existing
binary-upload policy and publisher `files` right. Transfers retain the portal's
ordered resumable parts, SHA-256 checks and staging quotas. `current` cannot be
uploaded as a file. Pending snapshots cannot be activated by the legacy commit.

`POST /<channel>/_push/common-commit` accepts exactly:

```json
{"snapshot":"<64 lowercase hex>","expectedCurrent":null}
```

`expectedCurrent` names the previous snapshot digest, or is null for a new
channel. The body is at most 1,024 bytes. The route uses the same publisher
accounts, channel scope, signed-request or HTTPS bearer authentication, push
policy and rate limit as existing uploads. A common commit runs the shipped
`apkg-portal-commit` helper using `Portal:PythonPath` (Python 3.11+ on POSIX).
The helper uses the file producer's exclusive writer lock, checked immutable
publication, bounded archive copying, directory flush and atomic reference
replacement. It uses `Portal:ApkgPath` for all common schema/cryptographic and
archive-content validation; there is no legacy verifier fallback.

The first validated commit registers its publisher account, channel identity
and signing public key in an immutable, non-served `.portal-owner.json` beside
the channel. Existing common channels without this registration require
explicit migration registration; another account cannot adopt their history.
Registered publishers use their stable GitHub account id, independently of
login or display-name changes. Operator-configured publishers use their
configured principal name. Locks and `staging/accounts/<identity-sha256>/...`
use the stable identity; display names never select filesystem locations or
bind common ownership. Old name-based staging is retained untouched for
explicit migration with account mapping; ambiguous mappings must refuse.
Later commits retain that binding, advance the sequence by exactly one and
include every previous build unchanged. Additional attestation evidence is
allowed; previously published evidence cannot be removed. Per authority/name,
author key replacement requires explicit operator migration. Publication
records the publisher account separately from the package author; mirroring a
valid signature does not prove account ownership of the signing key.

Every newly introduced descriptor requires its complete referenced archive.
The helper hashes it, imports it through the common engine in a disposable
validation root and checks that its embedded descriptor/signature exactly match
the snapshot. That private root's temporary author grant validates claimed
content only; it is never exported to clients. Retained descriptors were already
verified when admitted; each ordinary commit does not rescan their archives.
No arbitrary remote archive URL is fetched during this commit profile. The
legacy link-only publication path is retained; common publication does not
silently translate it. A common link-only workflow for self-service publishers
without the binary-storage right is a required remaining portal adaptation,
not a permanent dependency on the legacy engine or curator approval.

Only after all new packages pass does the helper retain immutable files and
replace `current`. Stale expected heads, history rewrites and sequence changes
return 409; owner/identity/key mismatches return 403; invalid new content returns
422. Retry of an already visible identical snapshot is idempotent, even with
the original expected head. Timeout, verifier failure or failed recording after
visibility may produce an `uncertain` outcome (503/504): inspect `current` and
retry the same signed request. The portal never blindly restores an older head.
Interruption may leave unreferenced immutable files or a verified owner binding;
they do not activate an incomplete snapshot and an identical retry completes it.

This profile assumes an operator-owned POSIX filesystem. It does not establish
power-loss durability, Windows semantics or multi-instance distributed storage
coordination. The OS file lock coordinates cooperative local producers as well
as portal processes; the portal additionally serializes publisher staging and
channel mutations in-process.

## Verified observation

`GET /api/common/channels/<channel>` returns a metadata observation containing
the snapshot digest, channel public key and complete common document. An absent
common reference returns 404, even if a legacy index exists. Invalid metadata
returns 422; an unavailable verifier returns 503; its timeout returns 504.
Responses are not cached. Error responses expose no local paths or tool output.

The reader bounds the reference to 65 bytes, metadata to 2 MiB and detached
signature to 16 KiB. It rejects links inside the configured data tree, verifies
the document digest against `current`, captures both files privately, and calls
`Portal:ApkgPath verify-object channel ...`. That common-engine operation checks
the channel schema and signature, every embedded descriptor and its signature,
duplicate build identities and embedded attestation contracts. The reader
rechecks `current` before exposing the captured observation. No legacy verifier
fallback is allowed. Executable output is bounded independently on stdout and
stderr; timeout, cancellation and excessive output stop and reap the process.

This is metadata verification only. It does not read package archives or grant
any author, channel, reviewer or tester role. The response reports signature as
`verified`, trust/review/runtime testing as `not-evaluated`, payload as
`not-checked` and freshness as `unknown`. Embedded attestations remain signed
statements requiring separate issuer authorization; their presence grants no
qualification. Complete authority/name, target, version and revision remain in
the document, including separate authors publishing the same name. Consumers
use the raw immutable channel URLs for signed bytes; the JSON API is a view.

The operator owns the data tree and verifier configuration. These checks do not
qualify hostile local filesystem writers, native handler durability or online
freshness. See [status](status.md) for completed integration and remaining work,
and [the plan](../../PLAN.md) for publication, catalogue UI and migration gates.


## Common catalogue and exact-build pages

`/packages/apkg` discovers listed common channels, with the existing viewer-key
rules for unlisted channels. `/packages/apkg/channels/<channel>` browses all
matching builds, with search over names, descriptions, categories, author
identities and declared file paths, plus exact author and target filters.
There is no implicit latest-version choice or same-name author merging. This
uses the existing reserved `/packages` namespace and does not reserve or break
a channel named `apkg`. The existing Pkg catalogue remains available.

`GET /api/common/channels/<channel>/packages` projects the same verified
observation with `q`, `authority`, `target`, `skip` and `take` (1–200, default
100). `total` counts matching builds before pagination. Revision, sequence
and payload sizes retain the common decimal-string contract. Signature,
trust, review, runtime, payload and freshness facts match the verified metadata
API's limits. The JSON response is a view: raw immutable URLs, not JSON
reserialization, supply signed bytes to clients.

Every build link uses its complete descriptor digest and a `snapshot` query
parameter. The historical document and signature are reverified through the
common executable; the page never substitutes the current head. Missing
snapshots/builds return 404, corrupt metadata 422, an unavailable verifier 503
and verifier timeout 504. Reads use `no-store`, including errors. These pages
display the signed record, claimed files and observations, and a CLI command
to preview the exact selection. Client trust must already be configured; a
changed channel requires a refreshed selection and new approval.

The portal does not order common versions with its legacy version comparer,
authorize embedded qualification issuers, grant keys, sign for authors, fetch
payloads while browsing or silently use a legacy index on verification failure.
The [checkpoint evidence](status.md#portal-common-catalogue-and-http-lifecycle)
records the real HTTP publication/consumption/offline-removal test and its scope.

## Portal execution batches

The operator authorized starting the port despite unsettled joint decisions.
The first lot is a concrete vertical slice; the remaining lots are not implied
passes. Production data stays outside the repository and deployment needs a
separately reviewable operational result.

| Lot | State | Remaining size | Deliverable and evaluation |
|---|---|---|---|
| PORTAL-BROWSE | **Host lot complete**; validation recorded in status | None for this host lot; AROS network follow-up stays separate | Common-engine-verified catalogue and exact-build pages; real authenticated HTTP publication, CLI/worker consumption, trust refusal, offline removal and exact retained-file inspection. |
| PORTAL-PUBLISH | Existing authenticated full-file commit is retained; common publisher client and link-only author mode remain | Several medium tasks | Preserve self-service accounts and resumable upload. An author without binary-storage rights can publish common signed content without curator approval. Test retries, owner/key binding, bounded external-content handling and conflicting heads. |
| PORTAL-QUALIFY | Signed observations are transported and remain unevaluated in this first UI | Medium, plus joint policy decisions | Evaluate independently authorized reviewer/tester evidence against exact builds; preserve revocation/history and distinguish signed observations, portal-admin statements and client trust. Agree official key custody/governance together. |
| PORTAL-MIGRATE | Not started; no production inventory or backup has been taken | Large; split inventory, fixture conversion, restore and rollback | Inventory accounts/rights, immutable content, channels, URLs, statistics and configuration. Copy and validate; retain originals and unknown provenance. Rehearse backup/restore and cutover rollback without a live switch. |
| PORTAL-OPERATE | Not started | Medium after migration evidence | Package common verifier/helper/runtime dependencies, adapt the deployment process and verify a disposable deployment. Review exact artifacts and operational inputs before a production cutover. |

Common architecture, trust governance and the first joint release's scope remain
proposals until both maintainers decide. The installed M1 `libpkg` updater and
its migration stay with their existing separate owner.
