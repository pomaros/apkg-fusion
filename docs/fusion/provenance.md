<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Source provenance

| Public input | Pinned revision | Imported material |
|---|---|---|
| [jonx/aros-pkg](https://github.com/jonx/aros-pkg) | [6940c7ad621993b866604b17c8248dcb949e52da](https://github.com/jonx/aros-pkg/tree/6940c7ad621993b866604b17c8248dcb949e52da) | Engine, portable services, CLI, portal, publishing, tests and documentation at the root. |
| [tomaszstaniak/arospkg](https://github.com/tomaszstaniak/arospkg) | [8e6ab1858b32bbf7a80f2f1c885a11af69aeb9f7](https://github.com/tomaszstaniak/arospkg/tree/8e6ab1858b32bbf7a80f2f1c885a11af69aeb9f7) | Native clients, bundle/relation logic, authoring/catalogue tools, tests and documentation under `components/arospkg`. |

Both projects publish their own code under MIT. John Knipper's and Tomasz
Staniak's original notices are retained. Third-party code retains its licences;
repository-level MIT licensing does not relicense dependencies. See the
[root licence](../../LICENSE),
[arospkg licence](../../components/arospkg/LICENSE) and notices beside each
dependency's imported sources.

The portable application-description integration derives from arospkg's RFC
0002 implementation and retains attribution. The explicit target module,
development CLI integration and authoring bridge are original integration work.
The bridge calls imported apkg-pack and the common engine's signing code.
Portal qualifications extend the imported publication and account model.

The [imported pkg guide](../../components/pkg-guide.md) preserves the original
root README with adjusted relative links. The
[imported arospkg README](../../components/arospkg/README.md) describes upstream
releases. Their download links and release statements refer to those projects;
[fusion status](status.md) owns this workspace's implementation evidence.

Private source trees, histories, credentials and product-specific integration
records are outside this provenance. Source ancestry does not imply that either
upstream maintainer has approved the fusion architecture or a release. The
[acceptance matrix](acceptance.md) records the work to agree and qualify before
a common release.
