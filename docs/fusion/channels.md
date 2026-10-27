<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signed channels

The common library implements authenticated discovery and role-separated policy.
The local producer now adds verified immutable file publication and an atomic
current reference. Context-owned host HTTP discovery and selected archive
transfer and journaled replay floors are implemented below. Online freshness, qualifications and
default-client integration remain ALPHA-CHANNEL work.

## Immutable discovery objects

A channel uses the common `apkg-json-1` envelope, type `channel`. Its body has
required `entries`, `id` and `sequence`, with optional `published_on`. The id is a persistent 128-bit lowercase
hex identity. Sequence is a positive uint64 decimal string. An empty entries
array is valid. Entries sort uniquely by exact descriptor SHA-256; this neutral
wire order is not version preference or channel priority.

Each entry requires:

- `archive`: `sha256` (64 lowercase hex characters) and `size` (uint64 decimal
  string, from 8 through 2,147,483,647 bytes).
- `descriptor`: the complete canonical signed descriptor object.
- `signature`: its complete detached descriptor-signature envelope.

The optional `publication` object carries publisher-reported application/build
first days. The [publication news contract](publication-news.md) specifies its
validation, preservation and shared recent-query semantics. These dates are
covered by the channel signature and do not establish online freshness, author
trust, review or runtime qualification. Optional `attestations` remain separate
signed observations as described below.

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
The [attestation profile](attestations.md) adds optional `reviewers`/`testers`
arrays with the same grant shape; every supplied role is displayed for approval.

A channel key authorizes discovery for its named channel id. It does not
approve package authors, targets, system writes, review or runtime testing.
An authority key authorizes package signatures for its named authority and does
not grant channel privileges. The operator may explicitly grant both roles to
one key/id; possession of one role never creates the other. Nothing updates
the policy implicitly, and no subscription installs keys.

### Changing the policy

Rotation, reset and revocation are one operation: `apkg_plan_policy`, or
`apkg policy ROOT POLICY-FILE`, replaces the whole policy with a complete
canonical policy object. Its presentation lists every grant added and revoked,
role by role, and every installed package whose signing key loses its
authority grant. Those packages stay installed; their author trust reads as
untrusted. The plan rewrites root state only, through the common journal:
installed records, channel floors and retained observations are unchanged, and
a plan made before any other change is stale. A rollback never crosses a policy
change, in either direction; reverting a change is another policy plan.

A producer's new key therefore reaches a root only when its operator approves
a policy naming it. A signed statement by the old key endorsing the new one
would let a client propose that plan, not skip it; that statement format is
not defined.

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
prove the latest online state. Reads enforce the root's explicitly accepted
sequence/digest plus any stricter caller floor. The view exposes
`accepted_sequence` independently; it is unknown before first acceptance.
Revocation observation and current review status remain separate facts.

## Root source list

A root keeps its own ordered list of named sources, each a channel identity at
an endpoint, as pkg's channel list did:

```sh
apkg source-add ROOT NAME ENDPOINT CHANNEL-ID [--confirm DIGEST]
apkg source-remove ROOT NAME [--confirm DIGEST]
apkg source-list ROOT
apkg source-install ROOT NAME [--recommends] [--confirm DIGEST]
```

The list lives in root state as an optional `sources` array and changes only
through a state-only plan that shows each source added or removed and the
resulting order. Adding a source first opens and verifies its channel under
the current policy, so it never stands in for granting the channel key.
Removing one leaves what was installed from it in place. Names are 1 to 63
letters, digits, `.`, `_` or `-`, starting with a letter or digit; names and
endpoint/channel pairs are unique, and a root lists at most 16 sources.

`source-install` reads every listed source in order and installs or upgrades
the named package. The newest trusted candidate for the root's target wins;
an equal version and revision goes to the earlier source. Dependencies are
pooled across the list under the same rule, and each package is prepared from
the source that offers it. Each source that supplies a package advances its own
replay floor and contributes its observations; the others are untouched. A
source that cannot be read, the same name from two authorities, and candidates
with no defined order refuse rather than narrow the choice silently. A package
that is already current yields an unchanged result. Rollback keeps the current
list, and a list change itself is never rolled back.

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
the producer alone cannot protect a reader from an older valid head. Readers
use the explicitly approved per-root replay floors below. This profile uses Linux/macOS POSIX locking, links, atomic replace
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
The inherited publishing bridge is now an explicit `--legacy` regression path;
common publication is the default, including in the installed helpers.

## Native opening and inspection

```sh
./build/apkg channel-open ROOT CHANNEL CHANNEL-ID MIN-SEQUENCE
./build/apkg channel-inspect ROOT SNAPSHOT-DIRECTORY CHANNEL-ID MIN-SEQUENCE
./build/apkg-core verify-object channel channel.json channel.signature.json
```

`apkg_channel_open_file` opens `current`, verifies the immutable snapshot and
its role/sequence policy, checks that the object digest matches the reference,
then rechecks the reference. It creates no files and fetches no archive.
Selected package decoding checks the reference again; a changed head requires
fresh discovery. Borrowed views remain immutable observations of that snapshot.
Plans retain owned source preconditions, as specified below. Shared client
selection also binds the discovered snapshot before any archive preparation.

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

1. Independent signed review/runtime facts, issuer policy and stale/revoked
   evidence handling; online freshness/revocation policy remains explicit work.
2. Interactive native catalogue/settings integration and consumer migration. The shared planner, discovery adapter and worker channel
   operation below are implemented host checkpoints.
3. Native transport/filesystem runtime qualification and retention/cleanup policy.

The final FUSION-ALPHA publishing/discovery/operation scenario and exact-source
Linux/macOS qualification remain required. Native execution, production portal
cutover and joint-release claims remain separate gates.

## Host HTTP discovery and selected archive transfer

`apkg_channel_open_http` reads the same endpoint members as the file reader,
checks both roles and the snapshot digest, then rereads `current`. Discovery is
read-only and creates no cache. Its observer reports one operation identity,
monotonic transferred bytes and an unknown total across the metadata requests.
The root's durable replay floor is enforced automatically. A caller can supply
a stricter floor without changing the stored value.

The Linux/macOS adapter launches host `curl` directly without a shell or the
legacy client's global request state. It disables curlrc loading, retains normal
TLS certificate verification, limits redirects to five, connection time to ten
seconds and each request to 120 seconds. HTTPS redirects cannot downgrade to
HTTP. The parent independently enforces byte bounds, polls cancellation, drains
bounded diagnostics and reaps its own subprocess. Metadata is bounded in memory;
archive data streams through 64 KiB buffers. Windows and AROS HTTP adapters are
explicitly unsupported. Local fixtures qualify HTTP behavior, not HTTPS server
interoperability or native networking.

`apkg_channel_fetch` requires a write context and a file/HTTP-bound channel.
It rechecks current source/root policy/target, transfers exactly the selected
archive and imports it through the common verifier. Whole-file size/SHA-256 and
exact selected descriptor/signature must all agree. It then rechecks source and
policy before retaining the archive. A valid archive belonging to another signed
package refuses even if the channel signed its whole-file digest.

Verified whole archives live privately at `<root>.apkg-archives/<sha256>.pkg`
with mode 0600 in a mode 0700 directory. Exclusive incoming files are flushed
before no-overwrite publication and directory flush. Cache hits are rehashed;
corrupt existing bytes are preserved and refused. Cancellation and ordinary
failures remove the operation's incoming file. A process killed during a copy
can leave unreferenced staging data; garbage collection is not implemented.
A failure after cache publication can leave a verified archive for safe retry.
Imported payload objects may also remain as preparation. Fetch does not commit
installed state or advance database generation.

The explicit CLI is `apkg channel-fetch ROOT ENDPOINT CHANNEL-ID INDEX
[MIN-SEQUENCE]`; `channel-open` accepts either a local directory or an HTTP(S)
URL. Observer phases distinguish checking from transfer. Selected packages
retain the source facts described below. The default production client switch
remains a separate alpha gate.

## Source-bound approved plans

Packages selected from file/HTTP channels retain owned origin records. Fetch
preserves those records after verifying the actual archive. Install, upgrade
and dependency resolution copy the origins of selected packages into the
canonical plan and its approval digest. Exact duplicates are folded in a stable
order; unused candidates impose no source obligation. Each record binds the
channel id, sequence, immutable snapshot digest, signer key, endpoint/transport, root id,
root/anchor paths and physical identity, policy digest and target. Paths are
hex-encoded local bytes, not executable commands. Raw immutable metadata supplied
directly to `apkg_channel_read` has no mutable endpoint to bind.

Planning verifies those facts. Execution rechecks reference, immutable object,
channel signature/role and current root policy during its CHECK phase, after
staging and before the commit decision. Snapshot bytes must still match the
original authenticated digest. Changed inputs require explicit approval of a
fresh applicable plan; network errors and cancellation do not authorize using
stale source facts. Checks run under the common mutation lock and bounded host
transport; they do not lock an external publisher. After the recorded commit
decision, source availability is irrelevant to completing local recovery.

Plan records own their bytes after channel/package objects are freed. Origins
remain in local plan/history records and are not copied into installed-state
records. Repair/removal and rollback from verified retained history therefore
work without a channel. Older journal plans without origins remain recoverable;
new origin fields are structurally validated without contacting a source during
recovery. Replay floors use the same journal through the following profile.

## Journaled replay floors

`apkg_plan_channel_accept` creates an explicit metadata plan for an already
verified channel. It grants no author/channel keys or independent qualification.
Approval and execution use the common root lock, journal, generation, recovery
and history. Reading, opening, selecting and fetching never advance a floor.
The current experimental CLI is `apkg channel-accept ROOT ENDPOINT
CHANNEL-ID [--confirm DIGEST]`: first invocation previews, the confirmed digest
executes. `channel-open` reports `accepted_sequence` or null separately from
unknown online freshness. Interactive native refresh integration remains open; approved channel install
also records the selected floor in its single common transaction.

Root bodies optionally contain `channel_heads`, sorted unique records with
exactly `id`, positive uint64 decimal `sequence` and `snapshot_sha256`. At most
1,024 channel identities are retained; exceeding the bound refuses rather than
forgetting older marks. Pre-floor common roots read as an empty registry. New
root generations retain it across all package operations. Policy changes or
endpoint changes do not implicitly reset it. Each accepted identity applies to
all its mirrors, independently of other identities or author trust.

Authenticated metadata below a stored sequence refuses as stale. A different
snapshot at the same sequence refuses as a conflict, including on another
mirror; equal sequence/digest is valid. Higher sequences can be observed without
writing. Only approved acceptance advances the persistent mark. Repeating an
explicit acceptance records another metadata generation while preserving the
same floor. It never silently lowers one. This protects persisted observations;
it does not prove that the observed signed head is the latest online head.

Root bodies also carry `package_generation`, a positive revision no greater than
the overall generation. Initialization starts both at 1; package operations
advance both, while channel acceptance advances only the overall generation.
Pre-extension common roots use their existing generation as the package
revision. This separates journal concurrency from package-history applicability.
A package rollback after catalogue-only updates still binds the exact current
root for execution, validates its selected history's intent/new-state hashes,
requires unchanged package revision/installed facts/policy/identity/target, and
preserves the latest replay registry. A subsequent package operation, including
rollback, invalidates reuse of that history. Restoring package state never
restores a lower catalogue floor. Attempting to use a channel-acceptance history
as a package rollback explicitly refuses; it cannot masquerade as undoing a
security floor.

Cancellation before decision restores or preserves the previous root. A
recorded decision is completed from local journal data even when the endpoint
is gone. Host tests exercise I/O failures, process death, interrupted recovery,
allocation failures and competing plans. Power-loss and native filesystem
qualification remain separate gates.

## Shared dependency-aware client workflow

`apkg_plan_channel_install` takes an already verified channel and an exact
requested descriptor digest. It checks the context/root and durable replay
state, builds the relevant metadata pool, then uses the existing resolver.
Compatible installed providers retain preference. Candidate ordering prefers
newer comparable versions/revisions and stays deterministic; conflict-driven
backtracking remains the common solver's responsibility.

Only selected new packages' archives are fetched and verified. Unselected or
already installed providers do not cause archive downloads. Metadata dependency
failure precedes archive preparation. The final canonical plan includes package
changes, installation reasons, selected source origins and the channel's new
replay floor. One approval, generation and journal commit all those facts.
Decline/cancellation/error before execution leaves installed state and floor
unchanged, while verified archive/object preparation can remain private.
Recovery/repair/rollback from retained content do not consult the channel.

The channel can contain up to its existing 4,096 rows. This workflow bounds the
relevant pool to one explicit request plus 128 candidate versions, with at most
128 installed packages and one million dependency-row inspections. Exceeding a
bound refuses; the client does not truncate the candidate set or silently omit
relations. Mandatory dependencies of existing installed packages are considered,
so replacing a provider does not ignore another package's needs. Recommendations
are included only when explicitly requested.

Shared convenience entry points live in `apkg_client.h`:

- `apkg_client_channel_open` returns an owned typed catalogue for interactive
  browsing, using the same read-only discovery as JSON queries. Channel and
  entry views remain valid after its temporary context closes. Names, versions,
  revisions and trust/target facts retain their complete common representation.
  `apkg_client_channel_selection` creates the exact selection string for a row;
  it never substitutes a name lookup or asserts installation authorization.
  Browsing and creating that string fetch no archives. A subsequent apply
  revalidates the observed snapshot and package authorization.
- `apkg_client_channel_query` performs read-only file/HTTP discovery and returns
  owned complete JSON facts, including blocked-author/target rows, accepted
  sequence, exact digests, unknown online freshness and unasserted independent
  qualifications.
- `apkg_client_channel_apply` accepts the exact selection string
  `CHANNEL-ID/SNAPSHOT-SHA256/DESCRIPTOR-SHA256`. A changed discovered snapshot
  refuses before archive preparation. It retains one plan through confirmation
  and never resolves again after approval.

The explicit common CLI uses:

```sh
./build/apkg channel-open ROOT ENDPOINT CHANNEL-ID
./build/apkg channel-install ROOT ENDPOINT CHANNEL-ID SNAPSHOT-SHA256 DESCRIPTOR-SHA256
# Repeat the same selection with --confirm PLAN-DIGEST after reviewing the plan.
```

`--recommends` opts into optional recommendations. Plan presentation includes
channel id/key/snapshot/floor plus package signer, content, ownership and
configuration effects. Author/channel trust does not assert review/runtime
qualification. The default `build/apkg` now uses the same common client; the complete native
interactive/client qualification gate remains open.

The actual PkgManager worker dispatches `APKG_CLIENT_CHANNEL` through the same
client function. Its advanced operation panel accepts the endpoint and complete
selection string. `pm_common_channel_query` shares complete owned discovery JSON
without reducing identities to legacy fixed-width table rows. Progress messages
copy all 128 operation-id bits, 64-bit event sequence/counters and phase. A failed
message allocation reports out-of-memory with the accurate filesystem outcome;
the final completion message is preallocated.

The fusion UI now includes a signed-catalogue window. The user supplies a
file/HTTP endpoint and channel identity, loads it on the DOS worker, searches
full names/authorities/targets, inspects independent author/channel trust and
target facts, and selects a row for install/update review. The worker copies
the catalogue into one bounded `AllocVec` allocation, including all strings,
64-bit revisions and exact selections; library handles die on the worker.
Blocked rows remain inspectable. The review button passes the selected
endpoint and exact snapshot/descriptor selection to the existing shared
plan/confirmation path. Independent review/runtime claims remain not asserted.

The UI owns the cancellation token until completion, allowing cancellation
during an HTTP wait without depending on another progress event. Closing the
catalogue window cancels an outstanding discovery. Failed/cancelled refreshes
retain the previous catalogue. Host tests exercise the actual discovery and
installation workers and allocation ownership; SDK header/type checks cover
the UI wiring. The main installed list now owns full common snapshot rows and
evidence, with complete name/version/revision display, search and action
identities. Installed LIST/SHOW and RUN/REMOVE have common ARexx source wiring.
ARexx INSTALL and UPGRADE with a package name run the
[root source list](#root-source-list) install from the worker, through the usual
confirmation window. ROLLBACK by name and legacy queries without an adapter
refuse explicitly. Native interaction, settings/news/filter/update views and
the remaining ARexx/compatibility cases still require integration/qualification.

The [attestation profile](attestations.md#channel-carriage-and-catalogue-presentation)
adds optional per-entry signed observation sets. They preserve review/runtime
claims independently of open publication, author trust and current approval.
