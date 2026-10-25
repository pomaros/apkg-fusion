arospkg 0.6.0 adds package relations, larger packages and ordered test releases.

## What's new

- **Relations.** Packages can depend on other packages (with a minimum version), recommend them, conflict with them and replace them. `apkg install` shows the plan first: dependencies are installed before the package, a replaced package is removed afterwards, and a package that another installed package needs is not removed. PkgManager shows the same plan.
- **Larger packages.** One file in a package may now be up to 128 MiB unpacked (64 MiB before). Files are unpacked through small fixed buffers, so a large package needs no more memory to install, and every file must match its recorded size and CRC-32 or the package is refused.
- **Test releases in order.** `apkg upgrade` and `apkg outdated` order versions with one `-testN`, `-previewN` or `-rcN` tag and a `-rN` rebuild tag: 0.9.0-test2-r2, 0.9.0-test4 and 0.9.0 come in that order, and test10 after test9. Versions in other forms are still refused rather than guessed.
- **What is new in the catalogue.** `apkg update` lists new packages and updates; `apkg recent` lists what was added recently. `apkg show` and PkgManager say whether a build was reviewed automatically and where it was tested.

## Compatibility

0.6.0 reads `index-v3.json`. Catalogue entries that need relations, or that contain a file over 64 MiB, are listed only there: apkg 0.4 and 0.5 do not see them, and would refuse or mishandle them. Everything else stays available to older clients.

## Downloads

| Build | Included | Status |
|---|---|---|
| [x86_64 ABIv11 / AROS One](https://github.com/tomaszstaniak/arospkg/releases/download/v0.6.0/arospkg-0.6.0.x86_64-aros-v11.zip) | apkg + PkgManager | Stable |
| [i386 ABIv0](https://github.com/tomaszstaniak/arospkg/releases/download/v0.6.0/arospkg-0.6.0.i386-aros-v0.zip) | apkg | Experimental |
| [x86_64 ABIv1 / mainline](https://github.com/tomaszstaniak/arospkg/releases/download/v0.6.0/arospkg-0.6.0.x86_64-aros-v1.zip) | apkg + PkgManager | Experimental |

There is no 0.6.0 build for the Raspberry Pi; use 0.5.0 there.

## Upgrade

From 0.4.x or 0.5.x:

```text
apkg self-update
```

This updates **apkg only**. Download the archive to update PkgManager.

From 0.3.x: unpack the appropriate archive and copy `apkg` to `C:`. Keep your existing `SYS:Packages`.

[User guide](https://github.com/tomaszstaniak/arospkg/blob/main/docs/guide/using.md)

Checksums are in the attached `SHA256SUMS`.
