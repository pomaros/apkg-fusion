<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# apkg-fusion

A common AROS package manager combining PkgManager's native desktop experience
with portable package operations, autonomous bundles, signed publishing, open
author channels and independent review/runtime evidence.

**This is a development alpha.** The default `apkg` executable and `libapkg.a`
use the common context engine. CLI and the PkgManager worker share signed
selection, dependency plans and transaction recovery. The native catalogue and
automation use the common engine; broader qualification, existing-user migration
and portal adaptation remain work. The [status overview](docs/fusion/status.md#alpha-progress-at-a-glance)
separates host qualification, the isolated M1 application trial and remaining
release gates. The [merge report](docs/fusion/merge-report.md) compares inherited
capabilities and fusion changes. Portal work is outside the current stop boundary.

The common engine in `src/apkg/` follows APKG's context model and links pkg's
portable services, cryptography, ownership and publishing code. The
[architecture](docs/fusion/architecture.md) and
[acceptance matrix](docs/fusion/acceptance.md) define the intended result.

## Build and test

Host builds need a C99 compiler and make. Authoring/integration needs Python
3.11 or later; host HTTP transport uses `curl` with normal TLS verification.

```sh
make
./build/apkg --version
./build/apkg --help
make test-fusion
sh tests/apkg/sanitize.sh
```

`make` produces `build/apkg` and `build/libapkg.a`. `build/apkg-core` is a
byte-identical development alias. `make install PREFIX=/chosen/prefix` installs
the common binary/library/headers and `apkg-build`, `apkg-channel` and
`apkg-publish`, including their authoring resources. `DESTDIR` supports staged
installation; the installed tools do not require the source checkout.

Imported `build/pkg`, `build/libpkg.a` and `build/apkg-provisional` are explicit
regression/migration baselines, excluded from the default product/install.
`make test-fusion` retains the provisional CLI and inherited publishing checks
in addition to the common default-product scenario. The full inherited engine
and client suites remain enabled in CI. The common Windows adapters are not
implemented; `build/apkg.exe` refuses rather than selecting a legacy writer.
Cross-build recipes and host tests do not qualify AROS/native runtime behavior.

## Author and publish

Use an application drawer and sibling `NAME.arospkg.toml` in the
[authoring format](components/arospkg/docs/guide/authoring.md). The common builder
preserves supported versions/revisions/relations, hidden files, empty drawers,
icons, configuration and declared Amiga metadata. Unsupported semantics refuse
explicitly. See the [common authoring profile](docs/fusion/authoring.md).

```sh
./build/apkg keygen /path/to/author.key
./build/apkg keygen /path/to/channel.key
python3 tools/apkg-publish /path/to/MyApp \
  --channel /path/to/channel --sign /path/to/author.key \
  --authority AUTHOR-ID --channel-id CHANNEL-ID \
  --channel-sign /path/to/channel.key --runtime native
```

Replace each identity with its persistent 32-character lowercase hex id. Keys
stay outside both the source drawer and public channel. CPU/ABI come from TOML;
the runtime is explicit. Publication builds/signs a PKG/1 archive and atomically
advances the immutable file channel. Authoring inputs stay unchanged. Declared
launch metadata produces `application.json` inside the packaged drawer.

Common publication is the default; `--common` is an optional explicit alias.
`--legacy --engine /path/to/pkg` selects the inherited bridge only for baseline
or migration work. Missing common identities never cause a legacy fallback.

Publication requires no curator approval. Author declarations and trusted
signatures do not assert independent review or runtime testing. The
[signed-channel profile](docs/fusion/channels.md) describes separate trust roles,
replay floors, exact selection and currently unasserted qualifications. Portal
and catalogue qualification integration remains a separate alpha gate.

## Initialize, discover and operate

Initialize a separate common root with an explicit target and canonical trust
policy containing the chosen author/channel grants. Initialization displays its
plan and confirms the exact displayed digest in the same terminal process.
Without a terminal it previews only; `--yes` explicitly authorizes the supplied
bootstrap target/policy for automation. Other mutations use the printed
`--confirm PLAN-DIGEST` token and refuse changed inputs.

```sh
./build/apkg init ROOT x86_64-aros-v11-native POLICY.json
./build/apkg channel-open ROOT ENDPOINT CHANNEL-ID
./build/apkg channel-install ROOT ENDPOINT CHANNEL-ID SNAPSHOT-SHA256 DESCRIPTOR-SHA256
# Review, then repeat that selection with --confirm PLAN-DIGEST.
./build/apkg list ROOT
./build/apkg repair ROOT PACKAGE-NAME
./build/apkg rollback ROOT RETAINED-HISTORY-DIRECTORY
./build/apkg remove ROOT PACKAGE-NAME
```

`ENDPOINT` is a local directory or HTTP(S) URL. Discovery reports complete
package/snapshot identities, trust/target facts and unknown qualifications.
Installation resolves metadata before fetching selected archives. One approved
common transaction commits package changes and the replay floor. Verified
private preparation can remain after decline; installed state stays unchanged.
Retained rollback and repair work offline and preserve personal configuration.

`bundle-check FOLDER TARGET` validates an unregistered bundle without running it;
`run-bundle` has its own platform/runtime requirements. Existing APKG installs
require the still-pending validated migration/rehearsal before replacement.

## Documentation and sources

- [Implementation plan](PLAN.md): sequence, dependencies and gates.
- [Architecture](docs/fusion/architecture.md): common responsibilities/contracts.
- [Acceptance](docs/fusion/acceptance.md): preserved capabilities and evidence.
- [Status](docs/fusion/status.md): tested checkpoints and remaining work.
- [Provenance](docs/fusion/provenance.md): pinned inputs and attribution.
- [Common engine/API](docs/fusion/initial-engine.md) and [operations](docs/fusion/operations.md).
- [Imported pkg guide](components/pkg-guide.md) and [arospkg guide](components/arospkg/README.md): inherited baselines.
- [Portal guide](portal/README.md): current portal service and tests.

The portal regression command is `dotnet test portal/tests/Portal.Tests`.
Host qualification and the isolated M1 application trial are separate evidence;
neither claims general AROS platform acceptance. No live portal cutover or OS
source modification is included. The projects' own code is MIT licensed; original and
third-party notices are preserved. See [LICENSE](LICENSE) and
[provenance](docs/fusion/provenance.md).
