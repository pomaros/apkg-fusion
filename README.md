<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# apkg-fusion

An integration workspace for a common AROS package manager: PkgManager's native
desktop experience, portable package operations, autonomous application bundles,
signed publishing, an open portal and a reviewed catalogue.

**This is a development build.** The common engine has an experimental CLI and
PkgManager worker path with signed plans, operations and transaction recovery.
The default client still uses the provisional backend; channels, existing-user
migration and consumer adaptations remain implementation work. See
[implementation status](docs/fusion/status.md) for tested behavior and release
requirements.

The source layout is provisional. Designs are selected by behavior, evidence
and maintainability, regardless of their original author. APKG's context-based core is the selected skeleton; pkg contributes portable
services, signing, ownership, system operations and publishing. Both clients
will use one engine. The [architecture](docs/fusion/architecture.md) and
[acceptance matrix](docs/fusion/acceptance.md) define the intended result.

## Build and test on a host

A C99 compiler and make build the native development client and library.
The integration tests also need Python 3.11 or later.

```sh
make
make test-fusion
./build/apkg --help
```

The default build produces `build/apkg` and `build/libpkg.a`. The separate
`build/pkg` target retains the imported command interface for baseline tests,
the portal and the authoring bridge during integration. It carries no promise
of compatibility for the finished product.

The portal uses .NET; its [guide](portal/README.md) covers local configuration
and its test suite:

```sh
dotnet test portal/tests/Portal.Tests
```

Host tests do not establish AROS runtime support. The release matrix includes
x86_64 ABIv11, i386 ABIv0, mainline x86_64 ABIv1, native Pi aarch64 and hosted
aarch64, plus macOS, Linux and Windows host tools. Each needs its own evidence.

## Author once, publish openly

The [signed-channel profile](docs/fusion/channels.md) adds common read-only
discovery, separate channel/author trust and atomic local publication through
`apkg-publish --common`. Network fetching and default-client integration remain
in progress.

The [common authoring checkpoint](docs/fusion/authoring.md) builds a signed
PKG/1 artifact directly from TOML and `.ameta` using `tools/apkg-build` and
`build/apkg-core`. Its common publishing mode uses the atomic file-channel
producer; the bridge below still uses the inherited development backend.

An application drawer and its sibling `NAME.arospkg.toml` use
[apkg-pack's authoring format](components/arospkg/docs/guide/authoring.md).
The bridge runs its technical validation, generates the bundle description and
asks the native engine to sign and publish the files. Source inputs stay intact.

```sh
make build/pkg
./build/apkg keygen --file /path/to/author.key
python3 tools/apkg-publish /path/to/MyApp --channel /path/to/channel --sign /path/to/author.key --runtime native
```

The key belongs outside the drawer and channel. The author declares CPU and
ABI in TOML; the command requires the runtime. The signed target is explicit,
for example `x86_64-aros-v11-native`. The untouched TOML is installed as
`MyApp/Package.arospkg.toml`; declared launch metadata produces
`MyApp/application.json` through apkg-pack.

Publication requires no catalogue approval. `depends_checked` records the
author's declaration. Independent review and runtime-test attestations have
separate authorities and apply to exact package content. The portal supports
these qualifications independently of publishing rights. A directory or share
is a usable channel; the portal is an optional publishing and discovery service.

The bridge supports application, tool and data drawers, numeric versions,
revision 1 and package dependencies. It refuses unsupported system requirements,
richer relations, distribution constraints, later revisions, prereleases,
shared system layouts, hidden payload files and empty directories. These are
explicit integration gaps, recorded in the acceptance matrix, so publication
cannot silently discard package semantics.

## Exercise the development client

```sh
./build/apkg install myapp --root /path/to/test-root --channel /path/to/channel --target x86_64-aros-v11-native
./build/apkg verify myapp --root /path/to/test-root
./build/apkg bundle-check /path/to/test-root/MyApp
```

`bundle-check` validates the description without registering the application.
It does not execute the application or establish runtime-library availability.
Native launching remains an integration requirement.

Existing APKG distributions require a tested migration path. The original pkg
API, commands and storage formats may change freely; their consumers will be
adapted to the common design.

## Documentation and sources

- [Implementation plan](PLAN.md): ordered work, dependencies and acceptance gates.
- [Architecture](docs/fusion/architecture.md): common responsibilities and contracts.
- [Acceptance matrix](docs/fusion/acceptance.md): preserved capabilities and evidence.
- [Implementation status](docs/fusion/status.md): tested work and remaining integration.
- [Source provenance](docs/fusion/provenance.md): pinned public inputs and attribution.
- [Imported pkg guide](components/pkg-guide.md): baseline engine and publishing workflows.
- [Imported arospkg guide](components/arospkg/README.md): baseline clients, bundles and catalogue.
- [Portal guide](portal/README.md): publication and independent qualifications.

The projects' own code is MIT licensed. Original notices are preserved;
bundled dependencies retain their respective licences. See [LICENSE](LICENSE)
and [provenance](docs/fusion/provenance.md).

The first [common context engine](docs/fusion/initial-engine.md) is available as
`build/libapkg.a` and the experimental `build/apkg-core`. It supports explicit
initialization, signed local-package preview/install and journal recovery, with
a PkgManager worker path enabled in fusion GUI builds. Its supported schema,
resource limits and platform qualification are documented separately from the
existing regression baseline. Run `make test-apkg` for its implementation tests.

The [common operations profile](docs/fusion/operations.md) documents removal,
repair, upgrades, offline rollback, local package dependency resolution, shared
client operations and unregistered bundle inspection/launch. Native execution
and legacy migration are qualified separately.
