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
twice. The aarch64 profile (`build-apkg-aros.sh`) still passes the flag and was
not rebuilt here.

AROS (x86_64 mainline nightly 20261105 in QEMU, `dos.library` 50.86): the
common CLI initializes a root and reads the signed channel; every installation
plan is refused, because `pkg_fs_atomic_replace_supported()` is false: the SDK
declares no `ReplaceObject`. Neither of the SDKs checked (official mainline
20261105 for x86_64 and raspi, ABIv11 2026.09) declares it. The refusal now
names that reason through the CLI and PkgManager
(`pkg_fs_atomic_replace_unavailable`); the measurement above predates that
change. A proposal for these systems is in
[proposal-aros-replacement.md](proposal-aros-replacement.md). PkgManager starts with Linktext views; copying
the release-page link put the exact 56-byte address on clipboard unit 0.
ABIv11 was compiled, not run.
