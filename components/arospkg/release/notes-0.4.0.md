Package management for AROS, with the apkg command-line tool and PkgManager desktop application.

## Downloads

| Target | Version | Included |
|---|---|---|
| [x86_64 ABIv11](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.0/arospkg-0.4.0.x86_64-aros-v11.zip) | 0.4.0 | apkg and PkgManager |
| [i386 ABIv0](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.0/arospkg-0.4.0.i386-aros-v0.zip) | 0.4.0 | apkg |
| [Raspberry Pi, aarch64 ABIv1](https://github.com/tomaszstaniak/arospkg/releases/download/v0.4.0/arospkg-0.4.0.aarch64-aros-raspi.zip) | 0.4.0 | apkg |
| [x86_64 ABIv1, mainline](https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1/arospkg-0.3.1.x86_64-aros-v1.zip) | 0.3.1, experimental | apkg |
| [Hosted aarch64, Macaros](https://github.com/tomaszstaniak/arospkg/releases/download/v0.3.1/arospkg-0.3.1.aarch64-aros.zip) | 0.3.1, experimental | apkg |

The Pi and Macaros builds are separate. Do not use one as a replacement for the other.

## What's new in 0.4.0

- Open an installed program's folder with `apkg open <id>` or **Open folder** in PkgManager.
- Read package setup notes after installation and in package details.
- See installation stages and which files were kept during removal.
- Browse larger catalogues. Tested with 5,000 entries on x86_64 ABIv11.
- Update apkg with `apkg self-update`. The previous executable is kept as `apkg.old`. PkgManager is updated separately.

## Install or upgrade

1. Download and unpack the archive for your target.
2. Close PkgManager, if running, and wait for any apkg operation to finish.
3. Copy `apkg` to `C:`, replacing the old executable.
4. Run:

```text
apkg --version
apkg update
```

Keep your existing `SYS:Packages`. On x86_64 ABIv11, start PkgManager from the unpacked folder.

**Upgrading from 0.3.x to 0.4.0 requires this manual installation.** In 0.4.0, use `apkg self-update --check` to check for future releases.

## Test coverage

- **x86_64 ABIv11:** full regression and release archive tested on AROS One 1.3.
- **i386 ABIv0:** catalogue download, search, install, open and remove tested under QEMU.
- **Raspberry Pi:** local install, show, open and remove tested under QEMU. Network access and downloads have not been tested for this build.

See `README.txt` for details. Use the `SHA256SUMS` from the matching release to verify your download.
