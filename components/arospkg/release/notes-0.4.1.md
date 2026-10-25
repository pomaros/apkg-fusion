Package management for AROS, with the apkg command-line tool and PkgManager desktop application.

## Downloads

| Target | Version | Included |
|---|---|---|
| [x86_64 ABIv11](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.1/arospkg-0.4.1.x86_64-aros-v11.zip) | 0.4.1 | apkg and PkgManager |
| [i386 ABIv0](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.1/arospkg-0.4.1.i386-aros-v0.zip) | 0.4.1 | apkg |
| [Raspberry Pi, aarch64 ABIv1](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.0/arospkg-0.4.0.aarch64-aros-raspi.zip) | 0.4.0 | apkg |
| [x86_64 ABIv1, mainline](https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1/arospkg-0.3.1.x86_64-aros-v1.zip) | 0.3.1, experimental | apkg |
| [Hosted aarch64, Macaros](https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1/arospkg-0.3.1.aarch64-aros.zip) | 0.3.1, experimental | apkg |

The i386 archive was added to this release on 2026-10-08, built from the same commit (v0.4.1). There is no 0.4.1 build for Raspberry Pi: one was built, but it could not be tested, so it was withdrawn. On a Pi, keep 0.4.0; `apkg self-update` there reports that this release has no archive for that machine and changes nothing.

## What's new in 0.4.1

- `apkg upgrade` moves to a newer version of a program, not only to a newer packaging revision of the same version. Example: `apkg upgrade limpet` takes Limpet from 0.1.0 to 0.2.0.
- The upgrade works as before: it shows a plan, keeps files you changed, refuses a conflict before changing anything, can be rolled back with `apkg rollback`, and is recovered if it is interrupted.
- Versions made of numbers and dots are compared as numbers, so 1.10 is newer than 1.9. apkg refuses an older version, the same number written differently (1.0 and 1.0.0), and versions it cannot order (such as `1.3-` or `v2`), and says why. For those, remove the package and install it again.
- PkgManager uses the same rule for its Upgrade button.

## Install or upgrade

On 0.4.0:

```text
apkg self-update
```

From 0.3.x, or by hand: download and unpack the archive, close PkgManager, copy `apkg` to `C:`, then run `apkg --version` and `apkg update`. Keep your existing `SYS:Packages`. On x86_64 ABIv11, start PkgManager from the unpacked folder.

## Test coverage

x86_64 ABIv11 on AROS One 1.3 under QEMU:

- the upgrade suite: revision upgrades, conflicts, rollbacks, interruptions at three points with recovery, and the new version cases (1.0 to 2.0 interrupted twice and recovered, then upgraded with the user's changes kept; an older, an equal and an unordered version refused; rollback from 2.0 to 1.0);
- Limpet from the public catalogue: 0.1.0 installed, a theme edited, `apkg upgrade limpet` to 0.2.0 with the edit kept, `verify`, rollback to 0.1.0 and upgrade again; the upgraded Limpet started.

i386 ABIv0 under QEMU, with the archive's own apkg:

- the same upgrade suite, with the test indexes set to i386: all checks passed;
- `apkg self-update` from 0.4.0 to 0.4.1.

Use the `SHA256SUMS` from this release to verify your download.

