Package management for AROS, with the apkg command-line tool and PkgManager desktop application.

## Downloads

| Target | Version | Included | Status |
|---|---|---|---|
| [x86_64 ABIv11](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.2/arospkg-0.4.2.x86_64-aros-v11.zip) | 0.4.2 | apkg and PkgManager | stable |
| [i386 ABIv0](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.2/arospkg-0.4.2.i386-aros-v0.zip) | 0.4.2 | apkg | tested under QEMU, see below |
| [x86_64 ABIv1, mainline](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.2/arospkg-0.4.2.x86_64-aros-v1.zip) | 0.4.2 | apkg | experimental |
| [Raspberry Pi, aarch64 ABIv1](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.2/arospkg-0.4.2.aarch64-aros-raspi.zip) | 0.4.2 | apkg and PkgManager | experimental; compile/link only |
| [Hosted aarch64, Macaros](https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1/arospkg-0.3.1.aarch64-aros.zip) | 0.3.1 | apkg | experimental |

The Raspberry Pi archive is an experimental build for user testing. Both apkg and PkgManager compile and link with Clang 20.1.0 and the native Pi SDK; neither 0.4.2 binary has been run on Pi, emulated or physical. The Folio checksum failure is not diagnosed. `--keep-rejected` is available to collect evidence. Adding this archive also makes it available through `apkg self-update` on Pi; that updates only apkg, and PkgManager must be taken from the archive. Users who prefer the previously tested CLI can retain 0.4.0.

[0.4.2 screenshots on AROS One](https://github.com/tomaszstaniak/arospkg/blob/main/docs/screenshots/0.4.2/README.md).

## What's new in 0.4.2

- **`apkg outdated`** lists installed packages for which the catalogue has a newer version, with the installed and the new version. An update that exists but cannot be installed on this machine is shown as blocked, with the reason: built for another CPU or ABI, a library the system does not have, or no checksum in the catalogue. Nothing is downloaded to make the list. `--all` lists every installed package with its state; `--json` prints one JSON object for scripts. Exit status 0 when the check ran, 10 when there is no readable catalogue.
- **`apkg update`** ends with the number of updates and the command that lists them.
- **`apkg upgrade --all`** first shows the plan, then upgrades the packages one at a time, each exactly as `apkg upgrade <id>` would, and reports each. A package that fails, for example because a file you changed would be overwritten, is left as it was and the others continue; the group is not undone as a whole. `apkg --dry-run upgrade --all` previews every package.
- **Ctrl-C** stops a download, including `apkg update`, and leaves nothing behind. Once files are being changed the operation finishes and apkg says the Ctrl-C came too late; `upgrade --all` then stops before the next package.
- **PkgManager**: an Updates (N) filter, the update state in the list, the installed version in the Version column (`1.0 -> 1.1` where an update exists), the date of the catalogue above the list, Cancel during Update index, and About > Check for a newer version. The ARexx port has a new `OUTDATED` command.
- **Mainline AROS**: output redirected to a file is complete. `apkg search >file` used to come out empty there because of a stdio fault in mainline builds before 2026-09-24.
- **`--keep-rejected`** keeps a download that failed its size or checksum test, for a bug report. Verification itself is unchanged.

`apkg self-update` replaces only apkg. Take PkgManager from the archive.

## Install or upgrade

On 0.4.0 or 0.4.1:

```text
apkg self-update
```

From 0.3.x, or by hand: download and unpack the archive, close PkgManager, copy `apkg` to `C:`, then run `apkg --version` and `apkg update`. Keep your existing `SYS:Packages`. On x86_64 ABIv11, start PkgManager from the unpacked folder.

## Test coverage

All under QEMU; nothing in this release was run on real hardware.

x86_64 ABIv11, AROS One 1.3 (pool slots v11-1 and v11-2), with the archive's binaries:

- the updates suite: nine packages installed from one catalogue, then a second catalogue gives each a different state (available, available with a conflicting user edit, current, newer installed, blocked by a missing library, versions that cannot be ordered, not in the catalogue, ambiguous, built for another ABI only); `outdated` as text, `--all`, `--plain`, `--json` and redirected to files; `upgrade --all` with the conflict refused and the user's files kept (checked by hash); rollback and removal; no catalogue, an unreadable catalogue, and a root with a space;
- Ctrl-C: during `apkg update` (catalogue kept), during a package download (cache empty afterwards, exit 10), and during the extraction of the first package of `upgrade --all` (that package finished, the second not started, exit 10);
- the public catalogue: `apkg update` and an install over the network;
- PkgManager: the Updates filter and Version column, Upgrade from the list through the preview and confirmation, Cancel during Update index.

i386 ABIv0 (pool slot v0-2), with the archive's apkg: the same updates suite (all checks passed), `apkg update` from the public catalogue, Ctrl-C during a download, and an install over the network.

x86_64 ABIv1, mainline nightly 2026-09-23 (pool slot v1-1), with the archive's apkg: the same updates suite, including every redirected output, `apkg update` from the public catalogue and Ctrl-C during it. The public catalogue has no packages for mainline, so no install over the network was made there.

Not runtime-tested in this release: PkgManager on anything but AROS One 1.3; both binaries in the experimental Pi archive. The ScreenBar integration remains an internal demonstration and is not included in release archives.

Use the `SHA256SUMS` from this release to verify your download.
