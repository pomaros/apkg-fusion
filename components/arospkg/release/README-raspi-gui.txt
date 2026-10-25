arospkg 0.4.2 for native AROS Raspberry Pi aarch64 ABIv1 — EXPERIMENTAL

Includes apkg, PkgManager and its icon. Built with Clang 20.1.0 and the native
Pi Developer SDK. Compilation and linking checked; these 0.4.2 binaries have
NOT been run on Raspberry Pi, physical or emulated. Available for user testing.
Do not use this archive on hosted Macaros or x86_64 AROS.

The reported Folio checksum failure remains unexplained. --keep-rejected
collects the rejected file and diagnostic hashes; it does not bypass verification.
The separate diagnostic archive supplied for Bohun remains unchanged.

INSTALL
Unpack, close PkgManager and let package operations finish. Copy apkg to C:.
Keep PkgManager.info next to PkgManager and launch it from the icon or Shell:
  Run PkgManager
Keep existing SYS:Packages. Network operations require bsdsocket.library and
ENV:SYS/Certificates/ca-bundle.crt. PkgManager needs muimaster.library; its
ARexx port is optional. apkg self-update updates only apkg, not PkgManager.

NEW IN 0.4.2
  apkg update
  apkg outdated
  apkg --json outdated
  apkg --dry-run upgrade --all
  apkg upgrade --all
  PkgManager --updates

CLI and GUI share the update model. GUI shows installed -> available versions,
blocked updates and their reasons. Upgrade previews protect modified files;
rollback and recovery are retained. Ctrl-C cancels downloads; once files are
changing the current transaction finishes before the group stops.

TO COLLECT A FAILED DOWNLOAD
  apkg --plain --keep-rejected install folio >SYS:folio-result.txt
Send folio-result.txt and the file named in the diagnostic output. A rejected
archive is never installed or reused as verified cache.

See BUILD.txt and SHA256SUMS for provenance and checksums.
Release notes: https://github.com/tomaszstaniak/arospkg/releases/tag/v0.4.2
Licences: LICENSE and licenses/.
