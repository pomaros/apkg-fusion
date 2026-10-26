<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signed channels

The common library implements authenticated discovery and role-separated policy.
The local producer now adds verified immutable file publication and an atomic
current reference. HTTP transport, durable freshness, qualifications and
default-client integration remain ALPHA-CHANNEL work.

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

## Atomic local publication

`tools/apkg-channel ARCHIVE --directory CHANNEL --id CHANNEL-ID --sign KEY`
publishes one real PKG/1 artifact. It verifies the entire archive through the
common engine in a private, discarded validation root. That root authorizes the
artifact's claimed author key only for cryptographic/content/CPU verification;
it grants no author/account/channel rights to a client. The producer never
exports an existing root's private object store or recovery history.

The file layout is:

```text
CHANNEL/current                              # 64 lowercase SHA-256 hex + LF
CHANNEL/archives/ARCHIVE-SHA256.pkg
CHANNEL/snapshots/CHANNEL-SHA256/channel.json
CHANNEL/snapshots/CHANNEL-SHA256/channel.signature.json
CHANNEL/.publish.lock
```

Archive transfer uses 64 KiB chunks, with length and SHA-256 checks during copy.
Metadata retains its 2 MiB bound. Objects and metadata files are flushed and
published exclusively; existing immutable files must match instead of being
replaced. Parent directories are flushed, including on retry. A partial,
unreferenced snapshot may have missing files after process death; retry fills
only the missing files and verifies existing ones. Corrupt existing files refuse.

The sole commit is an atomic replacement of `current`, after the complete signed
snapshot and archive are retained/flushed. The producer checks that the previous
reference still matches before replacing it. Cooperative writers hold a stable
OS file lock across catalogue read/modify/sign/commit; process death releases
that lock without deleting another writer's lock file. Two concurrent publishers
therefore retain both entries and advance the sequence serially.

Before this commit, interruptions preserve the previous complete head. After
commit, the new head names complete immutable material. A final directory-flush
failure reports that the reference was updated but durability was not confirmed;
it does not blindly restore an old head. Idempotent retry verifies the retained
artifact/snapshot and finishes flushing. A successful identical retry does not
increment sequence. The same descriptor with different archive/signature bytes,
or different content under the same build identity, refuses explicitly.

The configured channel id and current signing key must remain continuous.
Changing a key requires a separate rotation protocol, not implicit acceptance
of a new signer. Sequence advances relative to the authenticated current head;
this is not durable external high-water tracking or protection against a server
replaying an older valid head. Those remain separate consumer/publisher policy
obligations. This profile uses Linux/macOS POSIX locking, links, atomic replace
and flush operations. It does not certify Windows, network-filesystem semantics,
native DOS or power-loss durability.

Process death may leave owned private validation directories or unpublished
`.incoming-*`/`.current-*` files. Retry ignores unrelated preparations; it never
guesses that another writer has died or performs automatic garbage collection.

## One TOML publishing command

```sh
./tools/apkg-publish Drawer --common --channel CHANNEL \
  --sign author.key --authority AUTHOR-ID --runtime native \
  --channel-id CHANNEL-ID --channel-sign channel.key
```

The common mode builds privately with `apkg-build`, then invokes the atomic file
producer. Both signing keys must be outside the source and public channel;
source and channel trees must be disjoint. Original source bytes/modes remain
untouched. Output identifies the exact descriptor/archive/snapshot and asserts
open publication, with no independent review or runtime qualification.
The inherited default publishing bridge remains a regression path until the
separate default-client/consumer switch.

## Native opening and inspection

```sh
./build/apkg-core channel-open ROOT CHANNEL CHANNEL-ID MIN-SEQUENCE
./build/apkg-core channel-inspect ROOT SNAPSHOT-DIRECTORY CHANNEL-ID MIN-SEQUENCE
./build/apkg-core verify-object channel channel.json channel.signature.json
```

`apkg_channel_open_file` opens `current`, verifies the immutable snapshot and
its role/sequence policy, checks that the object digest matches the reference,
then rechecks the reference. It creates no files and fetches no archive.
Selected package decoding checks the reference again; a changed head requires
fresh discovery. Borrowed views remain immutable observations of that snapshot.
Plans made from already decoded package handles do not yet retain channel-head
preconditions: that belongs to channel-aware transfer/client planning.

The JSON inspection result includes descriptor/archive identities, author trust,
target verdict, unknown freshness, unchecked payload availability and unasserted
qualifications. `channel-inspect` reads a supplied immutable snapshot directory
containing `channel.json` and `channel.signature.json`; it is not a safe mutable
two-file publishing procedure.

`verify-object` checks metadata cryptography/schema without granting signer
roles or checking archive bytes. `archive-check ROOT ARCHIVE` explicitly imports
and verifies a complete artifact against an existing root policy; it may retain
verified blobs beside that root but changes no managed generation. The publisher
uses it only with its private validation root. No implicit root initialization or
trust expansion is performed by the importer.

## Remaining batches

1. Context-owned HTTP opening, bounded cancellable archive fetching/retention,
   archive-to-selected-descriptor binding, durable sequence floors and channel
   preconditions in plans.
2. Independent signed review/runtime facts, issuer policy and stale/revoked
   evidence handling, then shared discovery/selection through CLI and the actual
   PkgManager worker with dependency-aware fetching.

The final FUSION-ALPHA publishing/discovery/operation scenario and exact-source
Linux/macOS qualification remain required. Native execution, production portal
cutover and joint-release claims remain separate gates.
