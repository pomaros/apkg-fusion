<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# arospkg 0.7.5 to 0.7.8

Proposed on a review branch (`arospkg-0.7.8-overlay`, on top of
[`arospkg-0.7.4-linktext`](upstream-0.7.4.md)); not merged.
Upstream: [arospkg `2af02a4..ada7477`](https://github.com/tomaszstaniak/arospkg/compare/2af02a4...ada7477),
releases v0.7.5 to v0.7.8 and one later commit.

## Inherited sources

Applied to `components/arospkg` as upstream wrote them; release notes,
screenshots and the publish workflow file are left out. The inherited CLI keeps
its baseline version text; PkgManager's fallback version is 0.7.8.

| Release | Change | Files |
|---|---|---|
| 0.7.5 | Builds record the toolchain and SDK with the home directory as `~` | `src/build*.sh` |
| 0.7.6 | Archive members and roots whose paths do not fit are refused, not cut off; `u_join` returns -1 | `libpkg/core.c`, `zip.c`, `lha.c`, `util.c`, `tests/test_longpath.c` |
| 0.7.6 | Upgrade carries over the user's drawers and says when the drawer is in use | `libpkg/swap.c`, `tests/upgrade.script` |
| 0.7.6 | `publish_index.py` publishes from the last published catalogue | `tools/`, `tests/test_publish_index.py` |
| 0.7.7 | Upgrade and rollback keep the package drawer and move its contents | `libpkg/swap.c`, `recover.c`, `util.c` |
| 0.7.8 | `install` refuses a package the catalogue has only for another system | `libpkg/relations.c` |
| later | The hosted aarch64 build script is gone; i386 ABIv0 is not a release target | `src/`, `tools/make-rc.sh` |

Links inside text (0.7.5) were already on the base branch.

## The common engine

The native clients do not call the inherited code for mutations, so each
behaviour was compared with `src/apkg` by reading the code.

| Behaviour | Common engine | Action |
|---|---|---|
| Path does not fit | Paths go through `ak_path` (4096 bytes), which refuses; payload paths are validated before staging | The restore loop of recovery used three composed paths unchecked; it now stops (`changes.inc`) |
| Package for another system, chosen explicitly | Refused with `APKG_TARGET` before any archive transfer | None |
| Package for another system, installed by name | Was `APKG_NOT_FOUND`, "no listed source offers a trusted candidate" | Now `APKG_TARGET`, "the listed sources have this package built only for another system"; test `test_name_offered_only_for_another_system_is_refused_as_such` |
| Files and drawers the user added, empty ones included | Kept: `desired()` merges the old package, the new one and the observed tree | None |
| Owned file the user changed | The user's bytes stay | None |
| The package drawer itself | Replaced: a new tree is built and renamed over the name; the old tree moves into the transaction and then the history | Open, below |
| Drawer or file in use | No check and no message; a failed `Rename` is `APKG_IO` and the journal restores | Open, below |

## Open: the drawer's identity

arospkg 0.7.7 keeps the package drawer and moves its children, because on AROS
a lock on the drawer follows the object, not the name. A Shell path, an assign
added by a package startup script, or the `PROGDIR:` of a running program
points to the old tree after the common engine's update.

Observed on mainline x86_64 (nightly 20261113 runtime, `dos.library` 50.89,
`RAM:`, 2026-11-15): with `Assign HOLDDRW: <root>/trial/Drawer` and
`Assign HOLDPKG: <root>/trial` in place, `upgrade trial` from 1.2 to 2.0
returns 0 and the live tree is correct, with the changed file, the user's
drawer and the empty drawer preserved. Both assigns then name
`<root>.apkg-history.<id>/old-0` and `.../old-0/Drawer` and list the 1.2
files. `remove` afterwards also returns 0; the assigns still name the history
tree. Nothing is reported to the user in either case.

Keeping the drawer changes how a tree is staged
and restored, which belongs to the write and recovery contract, so it is
recorded here and not changed on this branch. A report that the drawer is in
use could be added where the live tree is renamed away.

## Results and limits

Host: `make test-fusion` passes with and without `ReplaceObject` (438 tests
each); the inherited arospkg host suite passes. The common-engine CLI builds
for mainline x86_64 (SDK of nightly 20261110), ABIv11 (2026.09) and the
Raspberry Pi (nightly 20261110). The held-drawer observation above is the only AROS run of this branch; the
rehearsal runs of the base branch are in [upstream-0.7.4](upstream-0.7.4.md).
