<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Clean common portal qualification report

The portal uses the merged APKG engine for publishing and discovery. It supports
common signed channels, self-service publishing, independent qualifications and
new-data backup/restore. Old pkg portal formats and deployed-data conversion
are not supported.
The server runs on .NET 10 and the native `apkg` executable, with no Python
runtime dependency. This server profile passes Linux and macOS qualification.
The [public Azure portal](https://aros-pkg.azurewebsites.net/) runs this common-only
service. Publishing is enabled; the migration warning remains on.

The .NET server starts its bundled executable without a shell, checks exit status
and bounded JSON, and cancels, terminates and reaps timed-out processes.

## What changed

- One common verifier/crypto/archive backend replaces the old portal package
  backend. The main catalogue, channel search and exact-build pages expose
  complete signed identities, targets, relations and file metadata.
- Self-service publishers retain stable GitHub account identity. New channels
  start unlisted and publish via external HTTPS archives; an operator may grant
  binary storage independently. Author publication needs no curator review.
- `apkg-portal-push` verifies signed local/remote heads, signs requests through
  the common CLI, uploads resumable chunks and atomically commits append-only
  snapshots. Retry, channel/account ownership and rights withdrawal are tested.
- The publisher retains HTTP affinity cookies and permits one fresh signed
  session after an authentication refusal. Permanent refusals stop the push.
- External archives are fetched under public-network/host/redirect/size bounds,
  imported by the common engine, and retained as immutable links for accounts
  without storage rights. Clients still verify the signed archive identity.
- Review and runtime observations stay separate from signatures, account rights
  and client trust. An optional operator common policy evaluates issuer grants,
  independence and linked revocation history. Policy withdrawal is reread;
  self-review is not independent; no current approval/freshness is fabricated.
- Every page prominently asks people not to use the service or its packages
  while the system is being merged with APKG, with an apology. The English notice
  is enabled by default and must remain enabled for deployment.

## Published-artifact qualification

The portal suite passes **108 tests** on Linux and macOS CI, including real socket
publication and common CLI/worker consumption, authentication/replay/refusals, account/admin
CSRF and role separation, exact historical browsing, qualification withdrawal
and the warning.
Three backup/restore tests also exercise the actual backup/restore CLI, corrupt
bytes, path traversal, private modes and replacement refusal.

The [Linux](evidence/portal-common/final-ci-linux-rehearsal.json) and
[macOS](evidence/portal-common/final-ci-macos-rehearsal.json) published-artifact receipts
record a successful synthetic application loop:

1. Start the published .NET application with its bundled common executable
   on an isolated local socket; verify health and the warning.
2. Publish signed v1 to stored and link-only channels using the resumable client;
   verify idempotent retry and exact-build browsing.
3. Install v1 through the CLI and actual PkgManager worker. Run the installed
   Python payload with the host interpreter; compare common installed state.
4. Edit configuration and add an unowned personal file. Stop the portal and take
   a private complete new-data baseline backup.
5. Restart the published artifact, publish v2 to both channels and upgrade both
   roots. Run v2 and verify configuration preservation and exact v1 URLs.
6. Stop both portal and external archive server. Roll back each client offline,
   damage/repair its program offline and run v1 again.
7. Uninstall through each client. Independently inspect the remaining tree:
   exactly the edited configuration and personal file remain; the installed set
   is empty, the declared empty directory is gone and no pending transaction or
   preparation remains.
8. Restore the baseline into a new data directory, restart the published
   artifact and verify health, publisher identities, owner pins and baseline
   signed URLs. No active data tree is overwritten.

[Browser evidence](evidence/portal-common/README.md) covers channel, build and
documentation pages at phone and desktop widths with no horizontal overflow.
The notice is visible in each rendered page.

## Reproduce and operate

See the [portal guide](../../portal/README.md) and
[operations procedure](../../portal/OPERATIONS.md). `apkg-portal-package` builds a
host-native framework-dependent artifact with a per-file SHA-256 manifest.
The CI portal jobs run the same deployed loop on Linux and macOS and archive
the tested native deployment plus its report. The artifact manifest binds the
source revision and every packaged file.

Backup captures new common channels, immutable content/link mappings, owner
pins, publisher records, staging and cookie keys with exact file modes. Stop
all writers first. Keep archives private; external configuration/secrets and
remotely hosted payloads need their own retention. Restore only to a new tree.
The operations guide records cutover checks and matching artifact/data/config
rollback, including accounting for writes after the baseline.

The [Linux](evidence/portal-common/final-ci-linux-recovery.json) and
[macOS](evidence/portal-common/final-ci-macos-recovery.json) process-recovery tests
kill the real published server before and after head visibility. Restart and
retry preserve the correct signed head without incoming-file residue. These
checks complement injected filesystem failures around durable publication.

## Deployment evidence

The [evidence index](evidence/portal-common/README.md) records the native
artifacts, file hashes, lifecycle and publication-recovery receipts. The server
package contains .NET assets and `apkg`; Python authoring, publishing, packaging
and backup tools run on the developer/operator machine.

## Live Azure qualification

The [live lifecycle receipt](evidence/portal-common/final-live-lifecycle.json)
records two signed versions published through stored and link-only channels over
public HTTPS. Both the CLI and actual worker install, execute the host payload,
upgrade, preserve configuration, roll back, repair and uninstall. Independent
inspection checks the exact remaining personal files and empty installed state.
The common client also installs through the linked archive redirect.

The [deployed-file receipt](evidence/portal-common/final-live-artifact.json) binds
every live application file to the tested Linux artifact. The
[final service receipt](evidence/portal-common/final-live-service.json) checks
health, HTTPS, the warning, active publishing and removal of temporary grants
and synthetic channels. The previous application/configuration backup and its
separate data tree are retained privately for rollback. Old pkg data is not
converted.

GitHub sign-in redirects to the provider with the correct HTTPS callback and a
correlation cookie. Completion of consent with a real account remains a manual
check; local authentication/account/administration regressions pass.

## Remaining boundaries

Linux/macOS published-artifact and live HTTPS checks establish the tested host
workflow. They do not establish complete live GitHub consent, an Azure slot
swap, distributed storage coordination or power-loss persistence. Host Python
payload execution does not qualify an AROS runtime. Native network/UI testing
and broader platform claims remain separate.

Compatibility with released APKG installations remains deferred to joint
discussion, as do official
key custody, shared governance and the first joint release scope. Development
evidence is not agreement by both maintainers.
