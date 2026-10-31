<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Common portal qualification evidence

Deployment receipts identify source `cd5c2044f52afc9e65d30654e8897a9a8d596f39`.
All identities and packages are synthetic. Private keys, account data,
configuration, authentication keyrings and backups are excluded.

- [Complete Linux/macOS CI gate](final-host-ci.json).
- [Linux CI deployment loop](final-ci-linux-rehearsal.json).
- [macOS CI deployment loop](final-ci-macos-rehearsal.json).
- [Linux real-process recovery](final-ci-linux-recovery.json).
- [macOS real-process recovery](final-ci-macos-recovery.json).
- [Deployment artifact identities](final-deployment-artifacts.json).
- [Downloaded archive verification](final-artifact-validation.json).
- [Live HTTPS lifecycle](final-live-lifecycle.json).
- [Live deployed-file verification](final-live-artifact.json).
- [Final live service and synthetic-data cleanup](final-live-service.json).

Each CI rehearsal used a clean source tree. Downloaded ZIP hashes, all packaged
file hashes, executable permissions and manifest/receipt bindings were checked.
The [qualification report](../../portal-report.md) describes coverage and limits.

[Viewport measurements](viewport-checks.json) cover channel, build and
documentation pages at 390 and 1280 CSS pixels. Every page displays the warning
and fits its viewport. Screenshots show the portal UI from `c414d4f`:

- [Phone channel page](channel-phone.png).
- [Desktop exact-build page](build-desktop.png).

The screenshots establish host-browser layout. Live-service checks have their
own receipts above; neither establishes AROS runtime support.
