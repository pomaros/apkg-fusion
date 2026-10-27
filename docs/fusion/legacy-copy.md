<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Legacy copy import rehearsal

The host adapter [apkg_migrate.py](../../tools/apkg_migrate.py) prepares a separate
copy of an arospkg root and registers its bundles through the common engine.
It never changes the original root, installs the new client over an existing
client, changes global settings, or switches a live installation to the copy.
Source records do not acquire retrospective publisher authentication.

## Prepare and approve

Build the common `apkg` executable. Supply an explicit public-key policy, target
and artifact mapping. The mapping is a JSON object with exactly these fields:

```json
{
  "format": "apkg-legacy-import-1",
  "legacy_root": "Work:Packages",
  "packages": [
    {"name": "map-demo", "artifact": "/absolute/path/to/signed-package"}
  ]
}
```

Every installed record needs one mapping. The artifact can be a signed package
folder or a PKG/1 archive. `legacy_root` scopes original DOS history references;
an empty label leaves those references unresolved. A recorded LHA archive hash
is retained in its original domain, never compared as a signed-manifest digest.

```text
python3 tools/apkg_migrate.py prepare ORIGINAL COPY mapping.json policy.json x86_64-aros-v11-native --quiescent
python3 tools/apkg_migrate.py commit COPY --confirm MIGRATION-SHA256
```

An optional global `--apkg PATH` selects the common executable. The operator must
establish source quiescence; `--quiescent` records that assertion and does not
claim to inspect global ENV/ENVARC locks or other machines. Source and destination
cannot overlap. The destination must not exist.

Preparation **creates the copy and initializes its explicit common policy**; it
is not a read-only command. The adapter inventories the source, copies complete
file contents and represented attributes, verifies the copy, and rechecks the
source. Original `db` bytes move within the new copy to `.apkg-legacy/db`.
Settings, cache/tmp, hidden files, empty directories, unowned icons and retained
history bytes remain present. Duplicate JSON keys, ambiguous identities,
unsupported required semantics, links, special nodes and unrepresented extended
attributes refuse. macOS provenance bookkeeping is excluded, as in the engine.

The common engine verifies the supplied artifacts, target, trust, recorded file
baselines and the combined dependency set. The adapter additionally compares
legacy package name, upstream version, packaging revision, recorded CPU/ABI and
known-empty relations with the verified descriptors. Unknown ABI, reason and
relations remain labelled unknown. Missing revision keeps its original omission
and the legacy reader's effective default zero separately; a matching supplied
artifact may carry revision zero. Zero is also a valid common dependency floor.

The immutable `.apkg-legacy/import.json` receipt includes source/copy inventories,
physical root identities, artifact facts, observations and the exact common plan.
Its SHA-256 is the migration approval. Receipts contain local paths and original
records; keep real receipts outside public source control.

Commit checks that exact receipt, copied data, source and artifacts remain
unchanged, then approves **that common plan**, without silently resolving again.
All package registrations commit together, using the same journal as ordinary
operations. The receipt is not a second transaction coordinator or commit marker.
No valid receipt is produced for a mismatched build or invalid signature.

## Interruption and subsequent operations

Before preparation returns a valid receipt, a stopped/failed copy is uncommitted
work. Preserve it for inspection and retry into a fresh destination; the adapter
never overwrites or automatically deletes a partial copy. The original remains
available. The copied root stays at its final path before common initialization,
so retained history does not need an unsupported root relocation.

After approval, recovery uses the common journal. The adapter automatically
recovers only a pending plan matching the approved receipt; another transaction
requires separate recovery. A pre-decision interruption restores the prepared
state and permits retry. Decided recovery finishes offline without source roots
or artifacts. Repeating an already committed import is an offline no-op.
Changes from later package operations invalidate the old import approval.

Use the common CLI/PkgManager after commit. [Adoption ownership guards](adoption.md)
protect edits and unowned paths through repair, update and removal. Common
verified-object retention and history may also create sibling engine-owned
paths beside the copy. Retained history supports offline rollback. Original previous-version records
and archives remain evidence; retained legacy bytes are not automatically usable
as common rollback history. History observations distinguish retained-unverified
bytes from unavailable/unresolved references.

## Profile and qualification boundaries

This profile interprets schema-1 installed records with file baselines. Nonempty
legacy dependency/recommendation/conflict/replacement lists and system
requirements require semantic adapters and explicitly refuse. Owned icons at
shared root destinations also refuse. The common adoption-set API itself supports
mutually dependent bundles; that does not establish translation of legacy
relation syntax. Unknown record fields refuse instead of disappearing.

Launch metadata and settings are preserved as original bytes; this adapter does
not reconstruct launch descriptors or activate old client settings. External
ENV/ENVARC associations, prior-version history conversion, old command/script
compatibility and real distribution capture remain migration work. Native
runtime, DOS/handler behavior and power-loss recovery require target evidence.
No native execution, M1 updater migration, portal migration or live cutover is
performed by this host rehearsal. See [status](status.md) for exact test scope.
