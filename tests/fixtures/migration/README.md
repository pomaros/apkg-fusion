<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Migration fixture inventory

See the [mapping](../../../docs/fusion/migration.md) and `inventory.json` for
source identities and provenance. All JSON files in this directory are newly
written synthetic examples under MIT, not data captured on a user's machine.
`cases.json` describes observations and expected classifications, not an engine
schema. `registry-current.json` follows the upstream schema-1 writer's field
shape; `registry-legacy.json` illustrates omissions understood by readers and
is not claimed to be output from a specific historical release.

Hashes of file contents in the edited-file case are real SHA-256 hashes of the
literal UTF-8 strings shown in `cases.json`. Archive hash/size describe a
fictional absent archive; they cannot establish archive validity. The
self-update hashes likewise describe the strings `old client` and `new client`.
DOS paths are inert fixture values. No real executable or archive is included.

The full upstream `components/arospkg/tests/fixtures/docs-walk-registry.json`
is referenced in place in `inventory.json`. Its original upstream licence and
notices remain in the imported component. It is documentation evidence;
independent distribution-capture provenance is unverified.

Still missing: real distribution roots and scripts, complete rollback bytes,
attributes/empty-directory inventories, persisted user settings, pending package
transaction captures, and older-version upgrade-route captures. Source code
and imported tests inform their required cases; these samples do not fill them.
