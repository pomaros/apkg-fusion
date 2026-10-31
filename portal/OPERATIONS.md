<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Operating the common portal

This describes the new common-only service. There is no conversion of deployed
pkg data. Retain the previous application, configuration and separate data tree
as a private rollback baseline.

## Artifact and host

Build `tools/apkg-portal-package --output <new-directory>` on the destination's
OS/CPU, after `make build/apkg`. It publishes the .NET application and ships
the native `apkg` executable. Python is used by developer/operator tools only;
it is not shipped or required by the server.
`deployment.json` records the source revision, dirty-worktree flag, native host,
engine contract and SHA-256 of every packaged file. Publisher tools run on the operator machine and accept
`--engine /absolute/path/to/apkg`; their source-checkout default is `build/apkg`. A dirty artifact is a local
candidate, not an exact-source release receipt. Do not deploy a macOS executable
to Linux. The CI Linux artifact is built/tested on Ubuntu 26.04 x86_64; this
does not prove it loads on an older Azure image. Check the exact destination
image, libc and runtime dependencies, or build inside that image before cutover.

When creating a deployment ZIP, preserve file modification times, bytes and
permissions. Incremental hosting deployments may use file metadata to decide
what to copy. Verify every deployed file against `deployment.json` after cutover.

The host needs the .NET 10 ASP.NET Core runtime. Serve the
published folder behind HTTPS; use a service supervisor and one operator-owned
POSIX data directory. Start with `dotnet /absolute/app/Portal.dll --urls
http://127.0.0.1:8080` from the published folder. A bundled executable is used
by default. `/health` checks the engine and every referenced signed channel.
Set the public URL to the external HTTPS address. Restrict direct access to the
backend port; the application recognizes the hosting HTTPS forwarding marker.

Operational inputs outside the artifact:

- `Portal__DataDir`: absolute persistent directory outside the application.
- `Portal__PublicUrl`: external HTTPS URL.
- `Portal__GitHub__ClientId` and `Portal__GitHub__ClientSecret`: OAuth credentials;
  callback `/signin-github`. Live OAuth requires a separate deployment check.
- `Portal__Admins`: semicolon-separated GitHub logins for browser administration.
- `Portal__AdminKeys`: independently scoped operator bearer-key hashes, generated
  by `dotnet Portal.dll adminkey <name>`; keep the printed secret privately.
- Optional `Portal__Keys` / `Portal__SignedKeys`: configured publisher grants.
  Self-service registration instead persists stable GitHub ids in the data tree.
- Optional `Portal__QualificationPolicyPath`: absolute common reviewer/tester
  policy. Back up this policy separately if it lives outside the data directory.
- `Portal__ShowMigrationNotice=true`: required until the operator clears it.

Never enable `Portal__AllowLoopbackHttpPush` on a production instance. It is an
explicit local rehearsal exception, including private HTTP archive links.
`Portal__Policy__Push=false` provides a read-only instance. `Binaries=off`
requires external links; `LinkHosts` restricts their public HTTPS destinations.
An empty `LinkHosts` permits any public HTTPS host. Size/staging quotas and
engine subprocess timeouts are configurable; see `PortalOptions.cs`.

## Backup and restore

Stop the service, publisher jobs and every other writer before copying. The
`--stopped` flag asserts this operational precondition; the tool cannot stop
unrelated writers. Run as the data owner. Keep backups private: they include
publisher records, staged uploads and authentication cookie encryption keys.

```sh
python3 tools/apkg-portal-data backup --data /absolute/data --archive /private/baseline.zip --stopped
python3 tools/apkg-portal-data restore --data /absolute/restored-data --archive /private/baseline.zip --stopped
```

The archive and restore destination must not already exist. Backup captures all
regular files and directories with modes, hashes and sizes; links/special files
refuse. Restore validates paths, membership and hashes before making a new tree
visible. Treat the ZIP hash as an integrity receipt, not an authentication
signature: store it through an independent trusted operator channel. Back up
external configuration/secrets, runtime versions and the exact application
artifact separately. Externally linked archives remain their publishers'
responsibility; a URL backup does not make remote content available offline.

Rehearse on a new data directory and an isolated port, verify `/health`, old
snapshot URLs, publisher/owner bindings and publication, then stop the rehearsal.
Never run the restored copy as a second writer to the active data tree. Local
backup tests cover process-level recovery; they do not establish power-loss
persistence, distributed coordination or native AROS filesystem behavior.

## Cutover and rollback procedure

Before a live cutover, review the exact-source Linux artifact, checksums,
configuration, HTTPS/OAuth callback, storage permissions, quotas and a private
backup/restore receipt. Keep the warning enabled on the candidate and after deployment.

For an approved cutover, stop writers, record the old application/data/config
identity, take a private baseline backup, and point the service to the reviewed
artifact and new data. Check health, notice, sign-in, read-only signed URLs and
a disposable publication before reopening writes. If a check fails, stop the
candidate; retain its data and logs. Restore the baseline into a new directory
and restore the matching application/configuration; verify health and signed
URLs before routing traffic back. Account for publications accepted after the
baseline before reopening writes; blindly restoring a head discards history.

The repeatable local command in [the portal guide](README.md#run-and-test)
actually stops and restarts the published artifact, takes a quiesced v1 backup,
advances to v2, restores a separate baseline, then verifies its signed URLs and
identities. It does not emulate a production reverse proxy, live GitHub login,
a real Azure slot swap or a downgrade between released server schemas.


## Live synthetic lifecycle check

`portal/tests/live_cycle.py` is an operator-side check for an owned HTTPS portal.
Prepare a fresh local work directory with `prepare --engine <apkg> --work <new-directory>`.
It creates private synthetic signing keys locally and prints only public publisher
grants. Temporarily configure those grants for the two named test channels;
make them unlisted and enable publishing. Refuse to proceed if either channel
already exists.

Run `run --engine <apkg> --worker <test_pm_common> --work <same-directory>
--url <https-portal>`. It publishes two versions through stored and link-only
channels, checks the current head before bounded same-snapshot retries
after uncertain responses, installs/upgrades through the CLI and actual worker, executes the host
payload, rolls back, repairs, removes and checks exact remaining files. It writes
`report.json` without private keys. This checks the deployed HTTPS service; the
separate published-artifact rehearsal establishes offline recovery and backup/restore.

After the check, withdraw the temporary grants, verify the synthetic owner pins,
and remove only those test channels, their visibility markers and their staging
identities. Verify health, the warning and an empty test catalogue. Keep real
accounts, production data and the rollback baseline intact. A successful sign-in
redirect does not establish completion of GitHub account consent.


## Qualified Azure profile

The 2026-10-31 deployment uses Linux x86_64, .NET/ASP.NET Core 10.0.14 and glibc
2.39. The tested native engine loads and verifies real publications on that
image. The container startup allowance is 600 seconds. HTTPS-only hosting and
the migration notice are enabled; production loopback-link exceptions are off.
Recheck runtime/image changes against the native artifact and public smoke test.
The [qualification report](../docs/fusion/portal-report.md) links file and lifecycle
receipts. Private rollback inputs include the previous application and
configuration; its separate data directory remains intact.
