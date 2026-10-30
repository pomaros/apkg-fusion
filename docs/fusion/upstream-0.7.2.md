<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Selective arospkg 0.7.2 integration

Review date: 2026-10-30. The imported base is `8e6ab185`; the selected product
source is [v0.7.2, `96eda125`](https://github.com/tomaszstaniak/arospkg/tree/96eda125f2ac1075604bfc893d9ac8f7c165da10).
The observed main head, `0e535a7`, adds screenshots only. Eleven upstream commits
follow the base; they do not change the selected common-engine architecture.
[Provenance](provenance.md) retains the base pin and identifies this overlay.

## Integrated behavior

- PkgManager opens or copies HTTP/HTTPS addresses through the upstream link
  modules. Opening is asynchronous through optional openurl.library; copying
  remains available without a browser. Pagination retains all collected links,
  and long or unsafe-to-launch addresses are copy-only.
- Common installed/candidate details, the signed-channel panel, common
  confirmation text and verification reports participate. Repository fields
  retain final punctuation exactly; raw descriptor JSON is not searched as
  prose. Link return IDs start at 2000, avoiding the common controls at 1001–1014.
  Confirmation text is still assigned after opening the window, preserving
  the native layout correction qualified on the earlier candidate.
- The inherited ZIP packer accepts members up to 128 MiB and keeps ELF execute
  bits. Added checks cover a real member above the previous 64 MiB limit,
  exact 128 MiB acceptance, larger-member refusal, the expanded-total cap,
  and data/program modes.
  Common PKG/1 authoring retains its own bounds and source/AROS mode semantics.
- The legacy self-update lock gate, safe version-first helper discovery and
  explicit AArch64 release-target selection are incorporated in the inherited
  sources. The common PkgManager still routes mutations to the common engine;
  its object audit rejects inherited mutation/self-update writer references.
  The fusion UI identifies itself as `fusion-alpha`, not as an upstream release.

## Common-lock review

Upstream's stale-file takeover needed a gate: two processes could otherwise
replace/remove a new live holder's file after observing the previous holder
was dead. Common host mutations lock the existing parent directory with
`flock`; they do not delete/recreate a lock file. Native AROS mutations use a
named Exec semaphore, found/created and acquired under `Forbid`, without a
stale-file takeover. The common code therefore does not need that specific
file-lock patch.

A new host regression stops an actual signed self-update while it holds the
mutation lock in private preparation. Another update and recovery both refuse
with busy and preserve the executable/state. After the holder is killed, a new
holder starts, and competitors still refuse it. After two such deaths, retry
commits exactly one whole release. This establishes the host behavior; it does
not establish native semaphore recovery after task death or power-loss behavior.
Those remain target qualification work.

## Validation and limits

Validation results are recorded in [status](status.md#selective-arospkg-072-overlay).
The new GUI browser/clipboard workflow needs an AROS runtime follow-up. The
previous M1 report identifies the earlier frozen source and does not qualify
this new GUI. The overlay does not resume portal work or promise migration
from a released APKG installation.

## Next shared decisions

Review the [maintainer alignment handoff](maintainer-agreement.md) together
before assigning further parallel implementation. Agree the skeleton and
journal, author publication versus independent review and official keys,
existing-user/migration scope and the first common release's clients/targets.
The [tool migration TODO](../../TODO.md) records the 0.7.1 helper-probe lesson and
older CLI/GUI replacement cases. No maintainer acceptance is recorded here.
