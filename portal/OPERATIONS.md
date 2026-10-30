<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Operating the common portal

This describes the new common-only service. There is no conversion of deployed
pkg data. Keep the old deployment and its data separately until production
cutover is reviewed. The installed M1 updater is outside this work.

## Artifact and host

Build `tools/apkg-portal-package --output <new-directory>` on the destination's
OS/CPU, after `make build/apkg`. It publishes the .NET application and ships
native `apkg`, Python validation helpers, the publisher client and data tool.
`deployment.json` records the source revision, dirty-worktree flag, native host,
engine contract and SHA-256 of every packaged file. A dirty artifact is a local
candidate, not an exact-source release receipt. Do not deploy a macOS executable
to Linux. Use a Linux build for the Azure Linux service.

The host needs the .NET 10 ASP.NET Core runtime and Python 3.11+. Serve the
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
helper timeouts are configurable; see `PortalOptions.cs`.

## Backup and restore

Stop the service, publisher jobs and every other writer before copying. The
`--stopped` flag asserts this operational precondition; the tool cannot stop
unrelated writers. Run as the data owner. Keep backups private: they include
publisher records, staged uploads and authentication cookie encryption keys.

```sh
python3 app/apkg-portal-data backup --data /absolute/data --archive /private/baseline.zip --stopped
python3 app/apkg-portal-data restore --data /absolute/restored-data --archive /private/baseline.zip --stopped
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
backup/restore receipt. The current local rehearsal authorizes no production
switch. Keep the warning enabled on the candidate and after deployment.

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
