<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common portal contracts

## Current operator scope

The portal supports common signed channels and new-data backup/restore.
Old pkg APIs, manifests, installers and deployed-data conversion are unsupported.
Compatibility with released APKG installations remains subject to joint design.

Every page displays an English notice asking people not to use the portal or
its packages during the APKG merge, with an apology for the inconvenience.
The notice is enabled by default and must remain enabled for deployment until
explicitly cleared by the operator.

Qualification covers stored and externally linked publication, signed requests,
independent qualification evidence, browsing, deployable artifacts and the
application lifecycle: install, upgrade, offline rollback/repair, uninstall and
exact retained-file inspection.

## Authenticated publication

Publishers retain separate author and channel private keys locally. The common
builder signs packages; the channel producer signs complete snapshots.
`tools/apkg-portal-push` verifies local/remote signed heads using `apkg`, requests
a session, plans missing members, resumes ordered parts, and submits
`POST /<channel>/_push/common-commit`. Signed requests use the common engine's
Ed25519 implementation; HTTPS bearer-token files are optional. There is no
old pkg verifier backend.

A commit accepts bounded JSON (64 KiB) with `snapshot`, nullable
`expectedCurrent`, and optional `links` mapping archive digests to HTTPS URLs.
`current` cannot be uploaded directly. Snapshot members and archives are
immutable. Stable GitHub account ids bind registered publishers; display names
never select staging paths or bind ownership. The first verified publication
pins account, channel id and channel key in private `.portal-owner.json`.
Existing unregistered heads refuse adoption. Later publications advance sequence
exactly once, preserve every build and signed observation and retain author
keys. Key replacement remains an explicit future policy decision.

New publishers are link-only and unlisted. Registration needs no curator's
package review. Operators independently grant storage/listing; neither grants
reviewer/tester status or client trust. Link-only rights are rechecked at commit,
including permission withdrawal after staging. Session/sequence/body binding,
channel scope, stable staging identity, quotas and resumable parts have tests.

Every new archive is imported by the common engine in a private validation
root and must exactly match its signed snapshot reference. The temporary author
grant validates content and is never exported to clients. External links are
fetched only at publication, with exact signed size/digest, per-archive and
aggregate bounds, all-public DNS checks, pinned checked socket destinations,
original-host TLS verification, host policy and bounded redirects. Credentials,
private destinations and HTTPS downgrades refuse. Only an explicit local test
setting permits literal loopback HTTP. Linked archives retain `.pkg.url`
references rather than payloads; raw downloads redirect there. Publishers
remain responsible for external availability.

The .NET publication service coordinates local file locking, immutable retention and
atomic head replacement. Retained builds need no repeated full import.
Conflicting heads/history return 409, owner/right mismatch 403, invalid content
422. An identical visible snapshot is idempotent. Failure after visibility may
return uncertain 503/504: inspect `current` and retry the same publication,
never blindly restore an old head. Helper scratch is removed on refusal,
success and timeout. Cooperative local POSIX writers are supported; host tests
do not establish distributed coordination or power-loss persistence.

## Verified observation

`/` lists visible channels. `/channels/<channel>` searches builds without
merging authors or selecting an implicit latest version.
`/builds/<channel>/<descriptor>?snapshot=<digest>` reverifies the exact
historical snapshot instead of silently using the current head.

JSON views are `/api/channels/<channel>`, `/packages` below it (search,
authority/target filters and bounded pagination), and `/builds/<descriptor>`
with the snapshot parameter. Unsigned-64-bit facts retain decimal strings.
Raw `/<channel>/current`, `snapshots/<digest>/channel.json`,
`channel.signature.json` and `archives/<digest>.pkg` supply the signed bytes;
JSON views are not signed-byte transports.

Readers bound/capture metadata, reject data-tree links and use
`apkg verify-object` for schema, channel/descriptor signatures and observations.
The current head is rechecked before return. Missing data returns 404, invalid
metadata 422, unavailable verifier 503, timeout 504, without private paths or
tool output. Reads use `no-store`; immutable raw files support ranges. Browsing
never fetches archives. Metadata verification grants no client trust, payload
validation, runtime qualification or online freshness.

## Common catalogue and exact-build pages

Exact-build pages display target, authority/name, full version/revision,
relations, declared file metadata, signer and a CLI preview selection. Earlier
exact URLs retain their identity after a new publication. Declared payload facts
remain distinct from live installation checks. Errors never fall back to
unsigned indexes or a second verifier.

## Independent qualification

Signed review/runtime records remain in immutable snapshots. Without
`Portal:QualificationPolicyPath` they remain explicitly unevaluated. Otherwise
the .NET qualification service captures the common policy and queries the common engine in
a disposable root. Only the already verified transport key is added there;
configured author/reviewer/tester grants are unchanged and no client root is
modified. Results identify the captured policy digest and each observation's
signer authorization, independence, result, sequence, target agreement and
linked-prefix history. Self-review is not independent; policy withdrawal is
rechecked on every request.

Revoked and failed observations remain visible alongside earlier records.
Current approval stays `not-asserted` and freshness `unknown`; there is no
fabricated latest approval. A broken role policy yields `unavailable` without
blocking publication or general browsing. Synthetic runtime fixtures exercise
the protocol, not an AROS runtime. Official key custody and joint governance
remain decisions for both maintainers.

## Portal execution batches

The [operations guide](../../portal/OPERATIONS.md) defines new-data backup,
restore and cutover rollback. Production cutover is a separate reviewed
operation; the installed M1 updater stays with its existing owner.

| Lot | Current state | Evaluation / remaining work |
|---|---|---|
| PORTAL-BROWSE | Implemented; host regressions pass | Default common UI, exact historical builds, search, metadata and signer facts; phone/desktop browser check passes locally. |
| PORTAL-PUBLISH | Implemented; signed stored/link-only workflows pass | Resumable client, stable ownership, external bounds, retry and rights withdrawal tested. |
| PORTAL-QUALIFY | Implemented; role/history regression passes | Self-review, revocation and policy withdrawal covered; official governance remains joint work. |
| PORTAL-MIGRATE | Deployed-data conversion removed by operator | New-data backup/restore tools pass; published-artifact restore rehearsal passes locally. |
| PORTAL-OPERATE | Host-native artifact implemented | Full deployed lifecycle passes locally; final Linux/macOS CI pending; no production cutover. |

See [status](status.md) for measured evidence. These batches do not imply joint
maintainer agreement or a release.
