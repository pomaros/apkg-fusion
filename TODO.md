<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Follow-up work

Implementation order and acceptance gates belong to [PLAN.md](PLAN.md).
These items are future work, not implemented capabilities.

- Review and document migration between versions of the APKG tool itself:
  supported starting versions, library/client and storage-format
  compatibility, migration ordering, preservation of installed packages,
  configuration, trust, sources and history. Define backup, interrupted-update
  recovery, retry and rollback behavior, including when a migrated format
  prevents using an older executable. Rehearse representative APKG-to-APKG
  upgrades with existing managed installations. Coordinate consumer adaptations
  with their owners; the installed
  M1 `libpkg` updater migration remains owned separately.

- Add `apkg diagnose` over the common engine, with human-readable and JSON
  reports usable from the CLI and PkgManager. Record the executable/build,
  target and runtime, DOS/handler identity, root binding, journal/recovery
  state, trust policy and source/cache configuration. Explain the failing
  operation and its OS error instead of returning an unqualified I/O error.
  Redact credentials and private source URLs in exportable reports. Default
  checks are read-only; filesystem mutation probes require an explicitly
  selected disposable directory and exercise identity under child creation
  and rename, locking, atomic publication and flush support there. Keep
  checks separate from `doctor --retry`, which performs recovery. A successful
  probe must not imply power-loss durability or qualify another handler.
