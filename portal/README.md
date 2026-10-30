<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# APKG common portal

The portal uses the merged common engine for signed metadata, archives and push
authentication. The default catalogue is `/`, channels are `/channels/<name>`,
and exact-build URLs include the immutable snapshot digest. This replaces the
old pkg portal without a compatibility layer or deployed-data conversion.
Joint maintainer design and official key governance remain provisional.

A prominent English migration warning is enabled on every page by default.
Keep `Portal:ShowMigrationNotice=true` until the operator explicitly clears it.
The public service and its packages should not be used during this transition.

## Publish

Create author and channel keys locally using `apkg keygen`. Register the channel
public key through the GitHub account page; never send either private key to
the server. New accounts publish by external link and start unlisted. An
operator may grant binary storage or list the channel independently of review.

```sh
python3 tools/apkg-build MyTool --output mytool.pkg --sign author.key --authority AUTHOR-ID --runtime native
python3 tools/apkg-channel mytool.pkg --directory channel --id CHANNEL-ID --sign channel.key
python3 tools/apkg-portal-push channel https://portal.example/my-channel --sign channel.key
```

Replace AUTHOR-ID and CHANNEL-ID with separate 32-digit lowercase hexadecimal
identities. `apkg key-public channel.key` returns the public key for registration.
Link-only accounts add `--links links.json`: a JSON object mapping each new
archive's SHA-256 to its public HTTPS URL. The portal fetches bounded bytes,
verifies the exact signed archive, then retains the URL rather than the binary.
Private addresses, unsafe redirects and digest/size mismatches refuse. Publishers
must keep their external archives available; backup retains URLs, not remote bytes.

The client signs requests with the common executable, sends resumable parts,
checks the remote signed head, and commits with an expected previous digest.
Retries of a published identical snapshot are idempotent. After an uncertain
response, inspect `/<channel>/current` before retrying; do not roll it back blindly.
HTTPS bearer-token publication is also supported with `--token-file`, a private
file. Credentials are not command-line arguments or source-controlled settings.

## Run and test

From the repository root with .NET 10 SDK, Python 3.11+ and a C compiler:

```sh
make build/apkg build/apkg-core build/test_pm_common
DOTNET_ENVIRONMENT=Production dotnet run --project portal/src/Portal
```

Set `Portal__DataDir` to an absolute, operator-owned POSIX directory and
`Portal__ApkgPath` to the common executable's absolute path for source runs.
The published artifact detects its bundled executable. Set
`Portal__GitHub__ClientId`, `Portal__GitHub__ClientSecret`, and
`Portal__PublicUrl` for real sign-in; the GitHub OAuth callback is `/signin-github`.
Keep secrets outside this repository. Without GitHub configuration the account
page explains that public registration is unavailable; browsing still works.

```sh
dotnet test portal/tests/Portal.Tests
python3 tools/apkg-portal-package --output build/portal-deployment
python3 portal/tests/deployment_cycle.py --app build/portal-deployment --work build/portal-rehearsal
```

Both output paths must be new. The rehearsal runs a real published portal with
synthetic identities and a host Python test payload, both common clients,
publication, upgrade, offline rollback/repair/removal, exact retained-file checks
and backup/restore. Host execution is not AROS runtime qualification. CI runs
this on Linux and macOS. See [operations](OPERATIONS.md) for deployment and
backup instructions, [contracts](../docs/fusion/portal.md) for limits and
[status](../docs/fusion/status.md) for measured evidence.
