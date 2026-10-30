<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common portal qualification evidence

Deployment receipts identify source `457b598b4db1b8256ca53f62b23a75ff165c70b2`.
All identities and packages are synthetic. Private keys, account data,
configuration, authentication keyrings and backups are excluded.

- [Local deployment loop](final-local-rehearsal.json).
- [Linux CI deployment loop](final-ci-linux-rehearsal.json).
- [macOS CI deployment loop](final-ci-macos-rehearsal.json).
- [Deployment artifact identities](final-deployment-artifacts.json).
- [Downloaded archive verification](final-artifact-validation.json).

Each rehearsal used a clean source tree. Downloaded ZIP hashes, all packaged
file hashes, executable permissions and manifest/receipt bindings were checked.
The [qualification report](../../portal-report.md) describes coverage and limits.

[Viewport measurements](viewport-checks.json) cover channel, build and
documentation pages at 390 and 1280 CSS pixels. Every page displays the warning
and fits its viewport. Screenshots show the portal UI from `c414d4f`:

- [Phone channel page](channel-phone.png).
- [Desktop exact-build page](build-desktop.png).

These are host-browser checks, not AROS runtime or live-service qualification.
