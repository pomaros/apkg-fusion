<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common authoring checkpoint

`tools/apkg-build` builds a common-engine PKG/1 artifact from the existing
apkg-pack drawer and sibling `NAME.arospkg.toml`. It is an ALPHA-AUTHOR
implementation checkpoint, separate from the provisional publishing bridge in
`tools/apkg-publish`. It creates no managed root, trust policy or channel.

```sh
make build/apkg-core
./build/apkg-core keygen author.key
./tools/apkg-build Drawer --output package.pkg --sign author.key \
  --authority 0123456789abcdef0123456789abcdef --runtime native
```

The authority is an explicitly chosen persistent publisher identity; it is not
inferred from a key or directory name. Publishing an artifact does not make that
authority trusted by clients. Root trust must independently authorize the exact
public key. `key-public` reports the public identity of an existing key.

## Source and metadata

`apkg-core source-inventory FOLDER` emits one canonical JSON observation on
success, without a status footer. It hashes regular files in chunks, records
explicit directories and ordinary modes, and interprets per-directory `.ameta`
using the existing portable C parser. This interpretation is confined to source
observation: installed-tree operations never guess that an arbitrary personal
`.ameta` is authoring control.

Malformed headers/lines, parser reports, stale names, reserved control names,
unrepresentable comments and conflicts between live semantic attributes and
`.ameta` refuse the complete inventory. Unknown extension key/value pairs remain
ordered opaque facts. The host execute bit controls the native execute-inhibit
bit, matching `pkg_ameta_publish_word`; other protection bits remain intact.
Controls are limited to 1 MiB each and 2 MiB cumulatively; inventory JSON remains
bounded to 2 MiB. Unsupported paths, links and unknown live metadata refuse.
The source wrapper's allocation tests instrument its own allocations; the
inherited standalone `.ameta` service retains its own allocator.

The source drawer is copied privately, then observed again before generated
files are added. A mismatch refuses the snapshot. The payload contains the
named drawer, any explicitly declared sibling icon, a byte-exact
`Package.arospkg.toml` provenance copy and the generated `application.json`
when `[launch]` is present. Hidden files and empty directories remain entries.
`.ameta` controls become signed semantic metadata, not ordinary payload files.
A mismatching pre-existing `application.json` refuses rather than being replaced.
Temporary owner-write permission allows generated files inside a private copy
of a read-only drawer; its original mode is restored before signing. Source
bytes and modes remain untouched.

The [host metadata profile](payload.md#host-semantic-metadata-profile) defines
protection, UTF-8 comments, Amiga ownership facts and opaque extensions. Parent
`.ameta` selection for the source drawer/sibling icon is not implemented: a
parent control file causes an explicit refusal. Nondefault metadata on the
source root itself also remains outside this profile. These are remaining
fidelity obligations, not permission to omit declared metadata.

## TOML and signatures

The builder reuses apkg-pack's application-description generator and catalogue
validation rules, without its ZIP extraction or whole-program reads. The common
native bundle inspector checks generated launch containment, target and program
headers using bounded streaming. CPU inspection is not runtime qualification.

Versions and revisions travel unchanged, including orderable prereleases.
Existing depends, recommends, conflicts and replaces map to common relations;
unqualified TOML names bind to the explicitly selected authority and target.
A missing minimum version means `0`, minimum revision `1`. Recommendation
reasons and descriptive metadata remain in the exact signed TOML provenance;
client presentation of that descriptive metadata remains ALPHA-CLIENTS work.
The new optional `config` list names relative files inside the drawer and maps
to the engine's `preserve` configuration policy. Missing, duplicate, directory
and control-file configuration entries refuse.

System requirements, distribution constraints and post-install notes with no
common execution/presentation path refuse. Unknown fields, including author
review claims, also refuse. Existing catalogue CPU/ABI/field/relation limits
still apply to this authoring checkpoint; broader common-engine API limits do
not imply that every legacy TOML consumer can represent them.

`keygen`, `key-public` and `sign-object KEY descriptor JSONFILE` use the common
executable's portable Ed25519 service. The existing `Pkg-Secret-Key: 1` seed and
public-key file format is retained. Damaged/noncanonical keys refuse; keygen
creates a private file exclusively and never replaces an existing key. Signed
JSON uses the same APKG-SIG domain framing consumed by the engine. No provisional
global request backend is used by this path.

## Artifact publication and remaining gates

Payload objects are streamed into a real PKG/1 envelope, rehashed during writing,
flushed, then published without replacing an existing output. Descriptor and
signature precede unique digest-addressed objects. The envelope's existing
2 GiB limit applies. An I/O failure before artifact publication removes its
private staging file. Process death can leave an unpublished `.apkg-build-*`
file; a retry ignores that file and never treats it as a channel publication.
No automatic stale-file deletion is implied.

This command emits an **unpublished** artifact. File/HTTP channel publication,
context discovery and qualifications are ALPHA-CHANNEL work. Connecting this
builder to `tools/apkg-publish`, replacing the default client backend, complete
metadata/timestamp coverage, legacy import and portal migration remain open.
The final FUSION-ALPHA scenario and exact-source Linux/macOS checks are still
required. No native program is executed by the host authoring fixtures.
