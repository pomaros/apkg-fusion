<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signed channel metadata checkpoint

This is the first ALPHA-CHANNEL implementation batch: authenticated discovery
and role-separated policy in the common library. It is not complete channel
publication, fetching, qualification or default-client integration.

## Immutable discovery objects

A channel uses the common `apkg-json-1` envelope, type `channel`. Its body has
exactly `entries`, `id` and `sequence`. The id is a persistent 128-bit lowercase
hex identity. Sequence is a positive uint64 decimal string. An empty entries
array is valid. Entries sort uniquely by exact descriptor SHA-256; this neutral
wire order is not version preference or channel priority.

Each entry has exactly:

- `archive`: `sha256` (64 lowercase hex characters) and `size` (uint64 decimal
  string, from 8 through 2,147,483,647 bytes).
- `descriptor`: the complete canonical signed descriptor object.
- `signature`: its complete detached descriptor-signature envelope.

Descriptor schema, embedded payload facts and package signatures are verified
before an entry is exposed. A repeated descriptor or two different descriptors
with the same authority/name/target/version/revision refuse the complete channel
as a conflict. Mixed authors and targets are permitted: they remain distinct
rows with explicit local authorization and target verdicts. Unknown fields or
required features refuse. Author-supplied review/runtime status fields therefore
cannot manufacture qualification through this schema.

A separate detached signature uses the existing 83-byte APKG-SIG transcript,
domain 2 (`channel`). Descriptor signatures remain domain 1. Signing and
verification share the same framing rules; no format/domain fallback occurs.
Metadata uses the context's object and working-memory bounds, with a maximum
of 4,096 entries. This is not an archive-content verifier: authenticating an
archive reference does not prove that its bytes have been fetched or retained.

## Root policy and role separation

Policy still requires `authorities`. It now optionally accepts `channels`, an
array with the same sorted `id`/`keys` shape. Keys are nonempty Ed25519 grants;
duplicate ids/keys and unknown fields refuse. Both role arrays are shown during
root-initialization confirmation and persisted in the exact approved policy.
Existing authority-only policies remain valid and grant no channel trust.

A channel key authorizes discovery for its named channel id. It does not
approve package authors, targets, system writes, review or runtime testing.
An authority key authorizes package signatures for its named authority and does
not grant channel privileges. The operator may explicitly grant both roles to
one key/id; possession of one role never creates the other. This batch adds no
implicit policy updates, rotation or subscription installation.

## Public context API and lifetime

`apkg_channel_read` copies the signed object and verifies its configured id,
channel signature and current root policy. The explicit minimum-sequence
argument rejects snapshots below the caller's known floor. The API creates no
files, fetches nothing, creates no root and performs no recovery. A ready root
with explicit trust policy is required.

`apkg_channel_get` and `apkg_channel_entry_at` expose immutable borrowed views
owned by the channel handle. Names and full descriptor/signature byte views
remain valid after input buffers and the original context are freed, until the
channel itself is freed. Views include exact descriptor/archive digests,
archive length, revision and separate signature/author-trust/target facts.

`apkg_channel_package` rechecks the root path/physical binding, root id, target
and exact policy before decoding a selected descriptor. Another root or changed
policy/target requires a fresh channel observation. Author and target checks
still run independently through the existing signed-package reader. Returned
packages own their bytes and survive channel/context release. This lookup does
not fetch an archive or promise blob availability; real transport and artifact
binding remain separate work.

Freshness is **unknown**. A valid signature and explicit sequence floor do not
prove the latest online state. The caller supplies the floor; durable high-water
tracking and revocation observation have not yet been implemented. This is not
a claim of rollback-resistant subscriptions or current review status.

## Native inspection and next batches

```sh
./build/apkg-core sign-object channel.key channel channel.json
./build/apkg-core channel-inspect ROOT SNAPSHOT-DIRECTORY CHANNEL-ID MIN-SEQUENCE
```

The inspection directory contains exact `channel.json` and
`channel.signature.json` bytes. Inspection emits one JSON result, with explicit
unknown freshness, unchecked payload availability and unasserted qualifications.
This is inspection of a supplied snapshot, not a safe two-file publication
procedure. Fixtures include descriptors from actual TOML/PKG builds and native
channel signing. They do not establish a production publisher or endpoint.

Remaining ALPHA-CHANNEL batches must provide:

1. Immutable file-channel snapshot/archive publication with one atomic current
   reference, writer concurrency control, interruption/retry and provenance.
2. Context-owned file/HTTP opening, bounded cancellable archive fetching and
   retention, archive-to-selected-descriptor binding, durable sequence floors
   and changed-input/qualification preconditions in plans.
3. Independent signed review/runtime facts, issuer policy and stale/revoked
   evidence handling, then shared discovery/selection through CLI and the actual
   PkgManager worker with dependency-aware fetching.

The final FUSION-ALPHA publishing/discovery/operation scenario and exact-source
Linux/macOS qualification remain required. Native execution, production portal
cutover and joint-release claims remain separate gates.
