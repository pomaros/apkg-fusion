<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Streaming payload transport

This is the transport portion of ALPHA-PAYLOAD, extending the
[initial engine](initial-engine.md) and [common operations](operations.md).
It is not completion of FUSION-ALPHA or of the full payload-metadata gate.
TOML authoring, network channels and the remaining metadata-fidelity obligations
still require their own implementation and evidence.

## Signed descriptor references

A regular file can use `blob` in place of inline `data`. All other file fields
retain their meaning: `mode`, `path`, `sha256`, `size`, `type`, and optionally
`config: "preserve"` and `amiga` (below). The blob value must equal its lowercase SHA-256. Sizes are
uint64 decimal strings. Blob references do not embed or allocate the file bytes.
Unknown fields and unsafe/colliding paths remain refusals. Inline descriptors
remain readable as bounded development/compatibility inputs.

`apkg_package_read` validates/copies the signature and descriptor; a reference
alone is not evidence that its content exists or matches the target CPU.
Import verifies every referenced object and its executable CPU evidence before
returning a usable package. Install/operation planning checks required incoming
blob content again; execution independently checks bytes copied to staging.
Cached content never grants author trust or independent qualification.

## Real AROS PKG/1 envelope

The reader uses the existing container's byte-order helpers and wire format,
with a streaming implementation instead of a whole-file buffer:

```text
PKG, version 1, big-endian total stream size
member descriptor.json
member signature.json
member objects/<lowercase SHA-256>
...
```

Each member uses the AROS path-length/path-NUL/data-length/data framing. The
first two members are mandatory and ordered. Object members must be uniquely
named and exactly satisfy the signed descriptor; shared references need one
object member. Missing members are refused even if the cache already holds
the bytes. Extra, duplicate, traversal, malformed, truncated and mismatched
members are refused. Nothing is extracted using an archive-supplied destination.
Directories, including empty drawers, come from the signed inventory rather
than being invented by extraction.

This uncompressed interoperable PKG/1 envelope is bounded to 2,147,483,647 bytes.
Compressed or other archive formats are explicitly unsupported here. This is
not a claim that the whole-stream inherited container API can handle larger
inputs or that distribution/image transport is finished. Existing descriptor
and memory budgets apply to metadata, not to payload file size; metadata can
still reach LIMIT. Streams are processed with a fixed 64 KiB I/O buffer.

The explicit `apkg_package_import` write-context API verifies metadata first,
then streams objects while reporting cancellable byte progress with an owning
import operation id and monotonic sequence. Progress is throttled; cancellation
is checked between chunks. No trust or managed database changes occur at import.
An import operation is separate from subsequent plan execution and has its own
operation id.

## Local retention and transaction use

Verified objects live beside the root in `<root>.apkg-objects/<SHA-256>`.
Creation is explicit preparation, never a side effect of opening a context,
reading a snapshot or making a pure plan. Client archive previews perform this
explicit import, so an unconfirmed preview can retain verified cache objects
while leaving the managed root/database unchanged.

Objects use private, exclusive temporary files, streamed hashing, file sync and
no-overwrite publication. Store and parent directories are flushed. Concurrent
imports may reuse an already verified identical object; a corrupt or linked
object is not silently replaced. A failed or cancelled import removes its own
private temporary file and can leave completed verified objects. Process death
can leave an unreferenced `.incoming.*` file; retry ignores it and revalidates
the complete transport. Import retry, not managed-root recovery, settles this
preparation. Garbage collection and stale-temp reclamation remain explicit
future retention-policy work; no cleanup guesses that another import has died.

Observed trees record hashes, sizes, paths and modes instead of duplicating all
file bytes in plans and journals. Explicit execution retains observed before-image
bytes, including edits and foreign files, before moving any managed participant.
Staging reads verified blobs in chunks. This supports large-file upgrade,
repair, offline rollback and removal, including configuration preservation.
Recovery verifies actual published/retained trees without materializing whole
files. Original inline inventories are normalized and checked for recovery
without rewriting their approved journal bytes or requiring a cache.

The object store can contain personal configuration and unknown before-image
bytes. It is private local recovery material, **not a publication directory**.
Future channel export must include only the explicitly authored signed artifact's
objects, never bulk-publish this store or its history.

All prior journal rules remain: one parent-lock domain, exact approval and
preconditions, retained evidence, rollback before decision and roll-forward
after it. Unsupported live metadata is still refused rather than stripped.
General timestamp and authoring fidelity remain required parts of
ALPHA-PAYLOAD. The host metadata profile below adds protection/comment/owner
facts; native execution/durability remains deferred.

## Host semantic metadata profile

A file or directory can declare an optional canonical `amiga` object:

```json
{"comment":"636166c3a9","extra":[],"gid":null,"protection":"2","uid":null}
```

All five keys are required when the object is present. `comment` is lower-hex
UTF-8, limited to the native 79 Latin-1 characters with no DOS control bytes.
`protection` preserves an unsigned 64-bit word as decimal text; its owner execute
inhibit bit (2) must agree with the POSIX owner execute bit. `uid` and `gid` are
null (unspecified) or uint32 decimal strings. They identify Amiga principals;
installing them on a host does not call `chown` or interpret them as host users.
`extra` preserves ordered opaque `.ameta` extension facts as objects containing
hex-encoded UTF-8 `key` and `value` strings. Reserved semantic keys cannot be
smuggled into extensions. Values remain inert and do not execute operations.
The complete metadata object is bounded to 16 KiB. Unknown semantic fields and
unrepresentable text are refusals.

On Linux/macOS the recognized `user.apkg.amiga` xattr stores this canonical
semantic object. The actual host execute mode controls the mirrored execute
inhibit bit on observation. An omitted or explicit default object means an
empty comment, no extensions, unspecified owners and only the mode-derived
execute inhibit bit. The xattr is a local fidelity carrier, not a trust mark.
Other xattrs, ACLs, special bits, foreign host ownership and links remain
unsupported before tree mutation. The top-level package folder's nondefault
Amiga metadata is refused until that folder has its own declared contract.

Files now accept ordinary POSIX modes through 0777 with owner read required.
Directories can declare an optional `mode`, default 0700, with owner read and
traverse required. Special bits remain unsupported. A declared mode is applied
exactly, never reduced to 0644/0755. These read/traverse requirements permit
verification and recovery under the current user's identity. Failed-preparation
cleanup may add owner permissions to private staging it is discarding; it does
not alter live trees or retained history.

Signed baseline, incoming baseline and actual metadata are merged independently
from file contents. A personal comment does not pin old program bytes during
upgrade. Changed protection bits are preserved individually; comment, uid, gid
and the ordered extension list each retain the actual value when edited since
the delivered baseline. Otherwise they take the incoming value. Removing an
otherwise pristine file with edited metadata retains it. Exact observations
bind approval, staging verification, retained before-images and offline rollback.
CLI and worker previews expose the resulting metadata facts.

This is host qualification only. Native mutation of descriptors with explicit
`amiga` metadata currently refuses in payload preflight pending adapter/runtime
qualification, including native owner/extension handling. The [common authoring checkpoint](authoring.md) now reads actual `.ameta` and
TOML files and supplies source/package/installed round-trip fixtures. Selected
parent controls and source-root metadata now reach explicit authored entries;
no unrelated sibling file content is read. Timestamps and broader fidelity
still remain unfinished ALPHA-PAYLOAD obligations. Direct-descriptor fixtures remain
separate from those new authoring cases.

Inventories allocate path strings to their actual length; directory-name arrays
grow geometrically within the metadata budget. The scale fixture installs,
repairs and removes 1,200 files while preserving a foreign personal file.

## Verification

The actual shared engine, CLI and native worker adapter consume generated real
Ed25519-signed PKG/1 archives. Tests cover payloads above the former inline
ceiling, small database/plan records, bounded individual engine allocations,
full large-file operations, hidden/empty paths, shared/zero-size content,
CPU mismatch, signatures/trust, malformed or incomplete transports, corrupt
cached objects and corruption after approval. Import cancellation, I/O failure,
process death, retry, allocation failure and old-format journal recovery have
explicit cases. Synthetic executable headers are never run.

ASan/UBSan includes the archive CLI/worker path. These checks do not prove
power-loss durability or AROS runtime compatibility. Measured results and the
remaining alpha gates belong in [status](status.md).
