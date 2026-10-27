<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common portal contracts

The portal serves the [common channel layout](channels.md#atomic-local-publication)
alongside the retained legacy channel files. A common channel has an explicit
`current` reference and immutable `snapshots/<sha256>/channel.json` and
`channel.signature.json` members. Archive URLs retain the common layout.
`current` is revalidated on every read; snapshot responses are immutable and
support HTTP ranges. Publishing these members through the legacy commit path
is refused. Common publication requires its own validated commit procedure.

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
