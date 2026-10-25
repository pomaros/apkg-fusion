arospkg 0.5.0 adds application launching from the Shell and PkgManager.

## What's new

- `apkg run <id> [files...]` and PkgManager's **Run** button launch installed applications. Workbench applications use their icon's stack and tooltypes; Shell tools return their exit code.
- PkgManager adds **Verify**, an **Installed** filter, and a list of files kept after removal. Installed packages remain accessible without a catalogue.
- Package authors can add a `[launch]` section to their manifest. `apkg-pack` validates it and generates the application description.
- Application pictures and a **Demo store** preview future GUI features. Purchases are not available.
- Ctrl-C during connection or TLS setup is now reported as cancellation.

## Downloads

| Build | Included | Status |
|---|---|---|
| [x86_64 ABIv11 / AROS One](https://github.com/tomaszstaniak/arospkg/releases/download/v0.5.0/arospkg-0.5.0.x86_64-aros-v11.zip) | apkg + PkgManager | Stable |
| [i386 ABIv0](https://github.com/tomaszstaniak/arospkg/releases/download/v0.5.0/arospkg-0.5.0.i386-aros-v0.zip) | apkg | Tested in QEMU |
| [x86_64 ABIv1 / mainline](https://github.com/tomaszstaniak/arospkg/releases/download/v0.5.0/arospkg-0.5.0.x86_64-aros-v1.zip) | apkg + PkgManager | Experimental |
| [Raspberry Pi aarch64](https://github.com/tomaszstaniak/arospkg/releases/download/v0.5.0/arospkg-0.5.0.aarch64-aros-raspi.zip) | apkg + PkgManager | Experimental; not runtime-tested |

## Upgrade

From 0.4.x:

```text
apkg self-update
```

This updates **apkg only**. Download the archive to update PkgManager.

From 0.3.x: unpack the appropriate archive and copy `apkg` to `C:`. Keep your existing `SYS:Packages`.

[User guide](https://github.com/tomaszstaniak/arospkg/blob/main/docs/guide/using.md) · [Screenshots](https://github.com/tomaszstaniak/arospkg/blob/main/docs/screenshots/0.5.0/README.md)

Checksums are in the attached `SHA256SUMS`.
