<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Clean common portal local evidence

The [local rehearsal receipt](local-rehearsal.json) records the first successful
published-artifact cycle from the development worktree. Its dirty flag is
explicit: the parent revision does not identify these uncommitted changes. The
later [clean local receipt](clean-local-rehearsal.json),
[Linux CI receipt](ci-linux-rehearsal.json) and
[macOS CI receipt](ci-macos-rehearsal.json) identify exact source `c414d4f`.
[Artifact identities and hashes](deployment-artifacts.json) bind the downloaded
archives to those successful rehearsals. See the [report](../../portal-report.md).
All identities and packages are synthetic. No private publisher key, account,
server configuration, authentication keyring or data backup is published here.

[Viewport measurements](viewport-checks.json) cover the channel, exact-build and
documentation pages at 390 and 1280 CSS pixels: every page has the warning and
none exceeds its viewport width. Screenshots were visually inspected:

- [Phone channel page](channel-phone.png).
- [Desktop exact-build page](build-desktop.png).

These show a host browser over the published local artifact. They are not an
AROS UI test or evidence of live deployment.
