<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# arospkg 0.7.4 and links inside text

Proposed on a review branch; not merged and not a maintainer agreement.
Upstream: [arospkg main `2af02a4`](https://github.com/tomaszstaniak/arospkg/tree/2af02a4),
which is release v0.7.4 plus the links merge `686faf2`. The
[0.7.2 overlay](upstream-0.7.2.md) remains the base of this change.

## PkgManager links

PkgManager shows text through Linktext views from
[zunekit](https://github.com/tomaszstaniak/zunekit) commit `03f6549`, copied
into `components/arospkg/third_party/zunekit` with file hashes in `PIN` and
compiled in (`-DLINKTEXT_EMBEDDED`). No system class is needed. A link stands
where it occurs in the text; the address rows, Copy buttons and Previous/Next
pages of 0.7.2 are gone. Opening stays asynchronous through OpenURL; addresses
over 480 characters or with `' * ; $` are copy-only; copying has no length limit.
Common inspection, row and verification reports go through `lk_doc_report`:
repository fields are exact and raw descriptor JSON is shown but not searched
for addresses. `--no-text-view` shows a Floattext fallback.

## Other changes on the branch

- ABIv11 has no `getentropy()`. With `PKG_ENTROPY_JENT` (set by `build-gui.sh`
  when its TLS layer uses jitterentropy) the common engine's `pkg_fs_random`
  reads that collector; a failed health test is a failure.
- `build-gui-mainline.sh` and `build-gui-raspi.sh` accept `APKG_FUSION=1`.
- The AROS rehearsal takes `--target` for x86_64 ABIv1 and ABIv11.
- Migration fixtures written by released arospkg 0.7.4 on AROS
  ([README](../../tests/fixtures/migration/captured-0.7.4-v1/README.md)), and
  installed records of every release line
  ([README](../../tests/fixtures/migration/legacy-records/README.md)).
- The legacy-copy importer refuses an unfinished replacement
  (`db/replace.json`), can inspect a copy of `ENVARC:arospkg`, keeps installed
  drawer icons as unowned user state when the mapping says so, and reads 0.1
  records without `subdir` ([legacy copy](legacy-copy.md)).

## Not ported

The 0.7.4 held-drawer fixes change the inherited `ops.c` install and remove,
which the native clients no longer call for mutations. The common engine moves
a removed package tree with `Rename` into its transaction, which AROS allows for
a held drawer; whether a later cleanup of that tree fails while another program
still holds it is not yet observed on AROS, because the x86_64 targets refuse
installation plans (below). The behaviour to keep: an empty drawer held by
another program never blocks reinstallation, is never reported as removed while
held, and is deleted once released.

## Results and limits

Host: `make test-fusion` passes; the inherited arospkg host suite passes,
including the new report cases. Builds, with the project profiles: ABIv11
PkgManager and CLI with GCC 10.5 against the 2026.09 SDK; ABIv1 PkgManager and
CLI with GCC 10.5 against the official SDK of nightly 20261105 (`--sysroot`);
Raspberry Pi PkgManager and CLI with clang 20.1 against the official raspi
SDK of nightly 20261105. All link without `--allow-multiple-definition`: the
container's status text is `pkg_container_strstatus`, so no symbol is defined
twice.

The aarch64 profile (`tools/build-apkg-aros.sh`) no longer passes the flag.
With `APKG_AROS_RUNTIME=native` against the official raspi SDK of nightly
20261105 (clang 20.1), `apkg-core` and PkgManager link without it. With
`APKG_AROS_RUNTIME=hosted` against the Macaros SDK (aros-upstream
`6a55c079cc`, clang of that workspace) the link finds duplicate
`__aros_libreq_DOSBase.0`, `PosixCBase.0`, `StdCBase.0` and `SysBase.0`: that
SDK's `AROS_LIBREQ` defines them `.global` in every unit, where current mainline
defines them `.weak`. The script links without the option first and repeats
the link with it only when every duplicate is such a symbol and the SDK header
is of that kind, listing them. A duplicate of our own still fails the build;
a two-unit test program with one checked that. Pomaros (hosted Linux aarch64)
was not built: no SDK of it is on this machine. These are builds only;
nothing was run on Macaros.

AROS (x86_64 mainline nightly 20261105 in QEMU, `dos.library` 50.86): the
common CLI initializes a root and reads the signed channel; every installation
plan is refused, because `pkg_fs_atomic_replace_supported()` is false: the SDK
declares no `ReplaceObject`. Neither of the SDKs checked (official mainline
20261105 for x86_64 and raspi, ABIv11 2026.09) declares it. Since `1de2818` the engine
replaces through the aside name instead of refusing; see "After the aside
path" below. A proposal for these systems is in
[proposal-aros-replacement.md](proposal-aros-replacement.md). PkgManager starts with Linktext views; copying
the release-page link put the exact 56-byte address on clipboard unit 0.
ABIv11 was compiled, not run.

With this branch (QEMU, mainline v1 and Pi nightly 20261105 with `dos.library`
50.86, ABIv11 2026.09 with 50.77): the CLI's install and channel install refuse
with "atomic file replacement is unavailable: this build's SDK declares no
ReplaceObject"; PkgManager shows the same reason when loading the signed
catalogue and, after `f48dc57` (checked on v1), when an install is refused.
PkgManager starts and closes cleanly on all three, and copying a link from its
About page puts the exact address on clipboard unit 0. No common-engine panel
with an address could be shown, because the catalogue does not load there. The
rest of the flow waits for the replacement decision.

## After the aside path (2026-11-14)

The branch was moved onto `1de2818` ("Replace state and profiles without
ReplaceObject"). The commit that named the refusal reason was dropped: the
engine no longer refuses.

The first run on mainline (x86_64 nightly 20261110, `dos.library` 50.89) got
past planning and stopped in preparation with `APKG_IO`. A staged file was
flushed with `fsync()` on a descriptor opened for reading, which the mainline C
library fails with `EBADF` (the fix is proposed upstream in AROS PR 2152, not
merged). On AROS `ak_sync_file()` therefore does not call `fsync()`. It first
asked the file's handler to flush; the handler's flush covers the whole volume,
so that repeated the same request for every staged file. It now does nothing on
AROS, and the `ak_sync_dir()` that ends `stage_package()` flushes the volume
once.

Installing one signed package of 402 files (400 of 4 KiB) on mainline x86_64
(`dos.library` 50.89, QEMU, one-second clock), flush per file against one flush:

| Volume | Per file | Once |
|---|---|---|
| `RAM:` | 8 s, 9 s | 8 s, 8 s |
| SFS | 12 s | 12 s |

Both builds return 0 on these volumes. Not measured on FFS: the test pool has no
FFS volume. On the pool's FAT32 volume both builds refuse the installation with
status 10 after about 28 s and an empty reason ("channel installation plan
refused: "); the cause is not established.

With that, the isolated rehearsal (`tests/aros/prepare_rehearsal.py`, 25 steps,
checked by `verify_rehearsal.py`) passes on all three systems, one boot each in
QEMU, everything in `RAM:`, replacement through the aside path:

| System | `dos.library` | Result |
|---|---|---|
| mainline x86_64 nightly 20261110 | 50.89 | PASS, 25 steps |
| ABIv11 2026.09 | 50.77 | PASS, 25 steps |
| Raspberry Pi nightly 20261110, non-SMP | 50.89 | PASS, 25 steps |

The steps cover root initialization, a signed channel install, refusal of a
tampered archive, upgrade, verification, removal with the edited configuration
kept, and the install through the GUI root. Not covered: a disk filesystem (SFS,
FFS, FAT), interruption between the aside steps on a real system, and a held
drawer.

All three CLIs link without `--allow-multiple-definition`. The ABIv11 build
reports one implicit declaration, `alloca` in the SDK's own
`clib/alib_protos.h`; none comes from this repository.

PomAROS SDK `sdk-aarch64-9b25c24`: `tools/build-apkg-aros.sh` now finds the
posixc headers of a published SDK (under `AROS/Developer/include`, without
`gen/`). The CLI compiles and links against it, with `ReplaceObject` declared.
The SDK's own `collect-aros` could not be used: it carries the absolute paths of
the machine that built it (`ld.lld`, `llvm-nm`, the library directory), so the
link used a locally built `collect-aros`. Nothing was run: no system of core
`9b25c24` is available here.
