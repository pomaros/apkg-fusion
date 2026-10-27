<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common data model

Status: CONTRACT data-model decision, revision 1. This defines the full logical
contract. The [initial engine profile](initial-engine.md) identifies the
implemented database/schema subset and its remaining restrictions.
It refines [architecture](architecture.md) using [MAP](migration.md).
The [public API draft](api.md) specifies ownership/lifetimes and event
interfaces; [serialization](serialization.md) specifies object/signature
framing. Journal ordering and durable writes belong to JOURNAL.

## Decisions and module ownership

arospkg's context owns root identity, queries, installed records and operation
state. Its existing registry reader becomes an explicit legacy adapter, not
the writer of common records. pkg supplies portable filesystem, manifest,
container/image, digest/signature, TLS/channel and publication services.
These services receive context policy and return typed data; they must not
retain mutable request globals or operate a second installed database.

| Object | Owner and source of truth |
|---|---|
| Root and committed database snapshot | Context/storage module, using portable filesystem capabilities |
| Package descriptor and payload inventory | Authoring/manifest module; exact immutable descriptor bytes |
| Author/channel identity and trust policy | Trust module; local policy plus verifiable authorization records |
| Installed package, ownership and reasons | Context database; committed transaction result |
| Live file/loader observations | Inspection module; observation time and environment, never a replacement for delivered metadata |
| Resolution and immutable plan | Common planner; pinned inputs and exact authorized effects |
| Transaction and recovery | One journal module, including self-update |
| Review/runtime attestations | Independent qualification module; clients and portal are producers/consumers |
| Legacy source snapshot and mapping | Migration adapter; retained original bytes and explicit evidence gaps |

The authoring source remains TOML, following the imported apkg-pack/RFC field
families. One validated author model produces descriptors and application
metadata. Original TOML may be retained as provenance; it is not a second
execution authority. Signed descriptor generation must include every
execution-relevant field. Editing any such field creates new content identity.

## Record primitives and evolution

All common records have an object type, schema major version and explicit
required-feature identifiers. Unsupported major versions or required features
prevent mutation; inspection can expose raw records and the refusal reason.
Unknown optional descriptive fields survive round trips and never authorize
action. Duplicate keys, truncated strings, numeric overflow, invalid digests
and contradictory fields are errors, not defaults.

Use unsigned 64-bit logical sizes, revisions, counters and generations; never
platform `long` as a persistent type. Wire encodings must preserve their exact
values, including above JavaScript's exact numeric range. Timestamps carry
UTC provenance and cannot establish operation ordering; generations do that.
The API/serialization task must specify bounds and typed limit failures before
allocation. No fixed-size client row may silently truncate authoritative data.

A possibly missing fact has `state = known | unknown | invalid`, a value only
when known, and evidence references. An invalid fact also has a diagnostic.
Known-empty lists differ from unknown lists. Evidence names a source object,
field and acquisition method: signed declaration, legacy record, exact-build
catalogue recovery, live observation or explicit operator decision. Evidence
alone grants no trust. Missing legacy fields retain their original absence even
when a compatibility policy supplies an effective value.

A content reference is `(algorithm, domain, digest, byte_length)`. Initial
algorithm is SHA-256. Domains distinguish descriptor bytes, archive/container
bytes, file bytes, legacy records and attestations. A digest of an archive
cannot stand in for a digest of its manifest. Exact bytes are hashed before
parsing and retained when signatures or provenance depend on them. Signing
transcripts and wire-format identifiers must be specified before SIGNED-READ;
existing signatures remain verified over their original bytes.

## Root and database identity

A common database lives under `<root>/.apkg/`. Legacy `db/` and `.pkg/` trees
are migration inputs, never alternate common authorities. The physical record
layout inside `.apkg` is storage-private until JOURNAL defines publication.
A root record contains:

- `root_id`: random 128-bit identity, allocated only by explicit initialization;
- `generation`: monotonic committed-state counter and committed snapshot digest;
- target and filesystem capability evidence;
- channel subscriptions, trust-policy revision and destination mappings;
- pending operation references and migration provenance.

A database snapshot binds root id, generation and all installed/ownership/policy
records that affect planning. A committed change advances generation; an
aborted operation does not produce a new committed snapshot. Generation
exhaustion refuses mutation. Pending operations are observable separately and
block ordinary mutation until recovery resolves them.

An open context also holds the resolved filesystem root identity/handle.
An assign name, path spelling, database UUID or copied UUID alone is not proof
that two handles refer to the same physical root. Rebinding or moving a root
requires an explicit operation and invalidates existing plans. Cloned roots
must receive a new root identity before independent mutation. Snapshot digest
and live preconditions guard against restored or edited database files with an
old generation. Root locks operate on physical identity, not aliases.

Reading an absent root returns absent with no id allocation, directories,
locks or recovery. Each root has one authoritative ownership map. Shared system
destinations are administered by their system root; a second application root
cannot independently claim the same external path. Multi-root execution and
cross-volume self-update require an explicit common-journal participant set;
until that protocol exists, such mutation is refused.

Migration preserves original bytes and references outside the new authoritative
snapshot, validates before cutover and records which legacy client entry points
must be replaced/disabled. Creating `.apkg` alone does not stop old clients
writing `db/`. MIGRATE must demonstrate exclusive cutover, retry and old-root
usability on failure before enabling common mutation.

## Package, authority and build identities

A package identity is `(authority_id, package_name)`. An authority id is a publisher-generated random 128-bit value carried in the
signed descriptor. It is a stable publishing identity bound by local trust policy to authorized signing keys;
it is not a URL, portal account name, display name or review status. Key
rotation changes authorized keys without changing the authority. An author
may establish a new authority without curator approval; accepting its key for
installation is an independent local trust decision. An unverified claim to
an existing authority grants no ownership of its packages.

Names retain original text for display/provenance and a validated canonical
ASCII name for lookup. New names are lowercase, start with a letter/digit and
continue with letters/digits, `.`, `_` or `-`; they cannot be `.` or `..`.
Legacy case variants map only when unambiguous. Foreign authorities using the
same name remain distinct; an unqualified user/dependency reference must resolve
unambiguously under an explicit channel/authority policy. Channel priority
must not silently transfer an installed package to another authority.

A build identity binds package identity, exact version text, packaging revision,
target constraints and descriptor content reference. A changed descriptor at
the same declared version/revision is conflicting publication, not a new
implicitly preferred build. Re-signing identical bytes does not change content
identity. Archives, mirrors and transport envelopes have separate references.
An installed instance has a local instance id, a build reference (possibly
unknown for legacy state), destination and reason. First implementation supports
one installed build per package identity per root; parallel versions require
explicit slots and separate destinations before they can be supported.

Legacy packages without verified authority/build identity receive a local
legacy identity tied to source-root snapshot and record identity. They retain
name/version/archive facts for inspection. Binding them to a signed authority
requires an explicit adoption plan and verified matching evidence; a matching
name or URL alone cannot claim them.

## Version and revision ordering

Store upstream version text unchanged, a comparison scheme and separate
packaging revision. New packages declare revision >= 1. Missing legacy revision
has effective value 0 with `legacy-default` evidence; it is not rewritten as 1.
The version suffix `-rN` belongs to the upstream version and is distinct from
packaging revision.

Scheme `aros-numeric-v1` adopts arospkg's existing comparison rules:
1–8 dot-separated decimal components, each 1–9 digits, optionally followed by
`-testN`, `-previewN` or `-rcN`, then optionally `-rN`. Tag numbers also have
1–9 digits. Missing trailing components and absent rebuild number compare as
zero. Compare numeric components first; at equal numeric components a final
release follows a prerelease, matching prerelease kinds compare their numbers,
and rebuild number breaks the remaining tie. Different prerelease kinds at
equal numeric components are unordered. Other strings use scheme `opaque`.

Comparison returns less, equal, greater or unordered. Byte-identical opaque
versions are equal for exact identity; distinct opaque versions are unordered.
An automatic upgrade is allowed only when upstream ordering is strictly newer,
or when version text is byte-identical and packaging revision increases.
Numerically equal but differently spelled versions require an explicit
replacement plan, even if revision increases. A downgrade/unordered replacement
requires a plan explicitly naming that action and retaining the same trust,
compatibility and preservation checks; no automatic fallback to text sorting.

Dependency minimums use the declared comparison scheme: numeric equality may
satisfy a numeric bound, while unordered cannot. Opaque exact-version constraints
require byte equality; minimum constraints on opaque versions are unsupported.
Revision bounds, when specified, apply to that exact upstream version and do
not substitute for version ordering. The [version corpus](../../tests/fixtures/contracts/versions.json)
records comparison and automatic-upgrade decisions, including legacy/default
and opaque cases. This intentionally retains arospkg's conservative upgrade
behavior while covering pkg's dotted-numeric ordering.

## Targets and system requirements

A concrete execution target remains CPU/OS/ABI/runtime, with exact canonical
`CPU-aros-ABI-runtime` spelling from [pkg_target.h](../../include/pkg_target.h).
A package declares either an explicit nonempty set of concrete targets or
`architecture-independent` data plus separately declared system requirements.
The latter is not a wildcard for executables. Actual ELF/Hunk CPU inspection
cannot establish ABI/runtime, and contradictory executable evidence refuses
publication/installation. Missing legacy target components remain unknown;
they are never filled from the publishing or migration host.

No cross-target compatibility is assumed. A future compatibility rule must be
versioned, explicit and included in the plan. Native, hosted and Raspberry Pi
profiles remain distinct. Imported ABI-only application requirements are retained
alongside the resolved target and their evidence. The
[target corpus](../../tests/fixtures/contracts/targets.json) covers concrete
syntax/matching; syntax acceptance does not establish a supported platform.

System requirements retain kind, identity, version constraints, target/loader
constraints and original declaration. Probe observations separately store
satisfied/missing/undetermined, observed versions, environment and time.
Installation-time success is history; launching may require a fresh probe.
An unregistered bundle uses its descriptor and actual environment without
creating an installed record or implicitly installing requirements.

## Payload and ownership

A payload inventory is an ordered collection of relative entries. Entry kinds
include regular file and directory; empty directories are explicit. Link kinds
must be explicitly described and supported by a target before use; unsupported
links or special files are refused, never omitted. Files include exact digest
and byte length. Entries retain path bytes/encoding, AROS protection word,
comment, available ownership metadata and preservation policy. Hidden names
are ordinary entries. Metadata that cannot be represented by the target is
reported before mutation; default behavior refuses lossy placement.

Paths are relative to a declared payload/destination base. Absolute paths,
volume/assign prefixes, traversal, empty components, embedded NUL and ambiguous
separator interpretations are rejected. The filesystem module checks actual
containment, link traversal and case/name collisions on the destination target.
Unsupported filename encodings fail explicitly; host Unicode normalization
must not silently rename source entries. `.ameta` is an authoring/transport
representation of attributes, not a license to drop unknown metadata or
ignore attribute errors during publication.

Each installed entry separates:

- delivered baseline: bytes and attributes from the installed build;
- ownership: installed/explicitly adopted, pre-existing-unowned or unknown;
- policy: ordinary payload, preserve user configuration, icon or generated data;
- live observation: matching, edited, missing, unreadable or unrecorded, with
  file identity/hash/attributes and the observation's provenance.

Upgrades compare old baseline, proposed baseline and live state. Modified and
unowned data survive; conflicts require a visible resolution in the plan.
A configuration side file such as `.pkgnew` is an explicit planned output with
collision handling. Directory membership does not imply ownership of all its
children. Adoption is an authorized ownership change, never inferred from a
matching filename. The `.apkg` database area and transaction staging paths are reserved: payloads
cannot own, replace or traverse them. Shared directories may have multiple claims; regular file
ownership is exclusive initially, even when two packages offer identical bytes.
Unknown legacy inventory coverage cannot justify deleting unrecorded entries.

Package kind is explicit: application, tool, data, system or image. System
placement uses declared logical destinations resolved through authorized root
mappings; it does not embed arbitrary host absolute paths in author metadata.
Images bind both image bytes and any declared inner inventory. Generated
`application.json`, icons and author-source copies must be included in the signed
inventory when shipped; live changes retain separate observations.

## Relations and installation reasons

Relations name their subject type: package or capability. Package references
resolve to an authority/name identity. An unqualified author reference is scoped
to the declaring authority by default; external authorities must be explicit.
Legacy unqualified dependencies retain their original channel resolution policy
until an explicit adoption mapping selects an authority.

Depends, recommends, conflicts, replaces and provides remain separate lists.
Unknown lists are not empty. Recommends are optional selections shown in a plan;
replaces grants neither provision nor permission to delete edits. Capability
identity includes kind (library/device/other), name and applicable ABI/loader
scope. A provider record names the actual delivered entry and capability
version; the package version is not assumed to be the library version.
A compatible installed provider is preferred. Plans record selected instances,
actual loader resolution and any provider change. Unknown metadata blocks
operations whose safety depends on it rather than making dependency checks pass.

Installed reason is explicit, dependency, recommendation, distribution-set or
unknown; store requesting instances/set membership separately. Unknown reasons
are retained by orphan cleanup. User promotion to explicit is a recorded state
change. Recommendation selection and set membership do not silently disappear
when a catalogue changes. Dependency graph updates commit with package state.

## Trust, channels and qualifications

The database keeps distinct signature verification, trusted-authority policy,
channel subscription, review and runtime evidence. Initial signing algorithms
reuse the existing SHA-256/Ed25519 implementations; signing new descriptor
formats requires an explicitly versioned transcript. Old payload/manifest
signatures remain verifiable in their source format.

The first [signed-channel metadata profile](channels.md) implements discovery
signature/role checks and exact immutable views. Host file/HTTP transport and
explicit journaled per-root replay floors are now implemented; online freshness,
revocation observation and independent qualification remain separate obligations.

A channel has a stable configured identity, endpoints, snapshot reference and
its own authentication/trust policy. Channel signatures authenticate discovery;
they do not replace package signatures. Local directory/share channels and
self-service portal publication use the same package identity rules. Stable,
testing and internal selectors are channel selection policy, not review grades.

Attestations bind exact descriptor digest, package identity, version/revision,
target scope, issuer, result, evidence and issuance/revocation history. Runtime
evidence additionally identifies the environment exercised. Review has no
implied runtime result. Author metadata cannot grant independent reviewer
privileges; an author-signed test report may be shown with its author provenance,
but cannot satisfy a policy requiring an independent tester. Changing content
or target cannot inherit qualification by name/version matching.

Offline state distinguishes verified cached evidence, unavailable evidence and
unknown freshness. Revocation observations and policy revisions are plan inputs;
missing network access is never represented as a fresh approval. Trusted-key
bootstrap/rotation/recovery and signed portable attestations remain QUALIFY
implementation obligations. Migrated portal accounts keep their authorization
mapping; account identity alone is not a signing-key or authority proof.

## Plans, history and recovery data

An immutable plan binds root id/physical binding, generation/snapshot digest,
trust/channel/compatibility policy revisions, exact build/content/signature
references, selected providers, live preconditions, destination mappings,
ownership changes, ordered effects, required privileges and retention needs.
A content digest identifies the plan's canonical immutable representation;
serialization and approval-token binding belong to the API/PLAN-INSTALL tasks.
Changes to any bound input require replanning; execution never re-resolves it
silently. An approval cannot be transferred to a different root or plan.

Database generation alone cannot detect user edits. Every affected path and
relevant loader/provider observation has revalidated live preconditions.
Filesystem guarantees against concurrent edits during mutation must be stated
by JOURNAL; root locking only excludes cooperating package-manager writers.

History records previous/current build identities, applied effects and retained
content references with availability: present-verified, missing, unreadable or
unknown. Offline rollback is offered only when every necessary byte and effect
is covered. Garbage collection cannot delete content pinned by a pending
transaction or promised retained rollback; explicit history pruning updates
availability before discarding the promise.

A transaction has a stable id, plan reference, participant roots/destinations,
commit decision and observed recovery outcomes. Self-update is a transaction
kind, not a second journal. Legacy arospkg journals and pkg staging/backups
retain source evidence and pending/unknown state until translated or resolved.
Old restart arguments/task pointers are inert history. Partial recovery and
manual intervention remain distinct outcomes; neither is reported as success.
Durable encoding, write ordering, activation/boot fallback and failure injection
remain JOURNAL/DISTRIBUTION tasks, not guarantees conferred by this document.

## Migration decisions and review cases

| MAP case | Required common representation/outcome |
|---|---|
| Edited `settings.cfg` plus foreign `notes.txt` | Original delivered hash retained; live edit and unowned file preserved |
| Missing prior archive/record | References retained, availability missing, rollback not promised |
| Missing ABI, reason and relations | Unknown facts with legacy source; effective revision 0 separately labelled |
| Kept-existing icon | Unowned and preserved, irrespective of package directory membership |
| Unrecorded hidden file/empty directory | Preserve observed entry; incomplete inventory is explicit |
| Interrupted self-update | Pending transaction evidence; ordinary mutation blocked; recovery required |
| Catalogue fallback from a different version | Descriptive evidence only; cannot become exact installed relations |
| Same name from another author | Distinct identity; explicit adoption/replacement and trust decision |
| Old root with live legacy clients | Cutover unavailable until exclusive access/compatibility route is established |

Review covered unknown-versus-empty, hash-domain confusion, authority/key
rotation, cloned roots, external system ownership, version ties, live edits,
rollback retention and self-update participants. The resulting constraints are
normative for the API draft. Review evidence and measured checks are recorded
in [status](status.md); this is not independent maintainer approval.

The API and serialization drafts complete the initial CONTRACT design scope.
Object-family schemas and required-feature gates must be frozen and tested in
their owning implementation batches before they can authorize operations.
The initial engine now implements context inspection, signed reads and a
journalled first install; broader mutation families remain separate work.
Real-distribution and filesystem evidence remains in the
[follow-up register](status.md#follow-up-verification-register).
