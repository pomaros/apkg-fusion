<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Last source refresh

The common news reader compares two authenticated observations of one channel.
It describes source additions and current-build changes, not changes to the
installed root or a complete source-withdrawal feed. A source withdrawal never
uninstalls a package. Source sequence/digests identify the observations; their
publisher dates do not establish online freshness.

## Comparison

`apkg_channel_news` checks the current channel against the captured root
snapshot. It authenticates previous bytes under that snapshot's channel policy
and checks the complete metadata/signatures with the same decoder and bounds.
Different channel identities, invalid signatures, withdrawn signing grants,
future baselines and same-sequence forks refuse. A historical observation may
be below the accepted replay floor for this comparison, but its private handle
cannot select packages, prepare package metadata for resolution or advance a
replay floor. Normal channel reads retain their replay checks.

Current-target groups use the shared recent-query version/revision selector.
Indeterminate comparisons are counted rather than ordered by spelling. Results
distinguish a newly advertised application, an application gaining this target,
a changed version/revision and changed descriptor/archive/signature facts for
the same release. An older advertised version is a source observation, not an
authorization to downgrade an installed package. Date-only and qualification-only
changes are not reported as package-version changes.

Without a previous observation, the result says that comparison is unavailable
and supplies the labelled recent-publication fallback used by the original
desktop behavior. It never calls every existing application new. A valid
comparison with identical snapshots yields an empty delta. These cases remain
different in the API, JSON and desktop header. Earlier-publication flags use
publisher dates and retain unknown/not-applicable values; they are not proof
that this machine previously installed or observed the application.

The result borrows current package views from the current channel, which must
outlive it. It owns the decoded historical observation and its prior-version
views; the root snapshot can be released after construction. Formatting returns
owned text. CLI and worker outputs retain complete identities, revisions,
previous versions, directions and source fingerprints.

## Profiles and persistence

Source profiles without history keep the existing version-1 frame. Version 2
adds previous-object/signature lengths to the big-endian framing and carries
the previous signed observation after the current pair. Both versions remain
readable. Invalid lengths, mismatched pairs, missing current data, unknown
versions, trailing bytes and oversized files refuse. Profiles remain private
atomic-file replacements, bounded at 16 MiB. Framing alone grants no trust.

An explicit refresh authenticates the comparison before replacing the profile.
Comparison failure preserves the existing file. A successful operation returns
an owned capture of the frame it wrote; another concurrent save cannot mutate
that capture. Concurrent writers remain last-wins, with each frame retaining
its captured baseline rather than claiming a globally serialized observation
history. No root trust, registration or replay floor is changed.

`source-cache` and a desktop network refresh with a selected profile advance
the observation pair, even when the source snapshot is unchanged. The latter
therefore records a real zero-change refresh. A display filter or offline
recomparison retains the pair. Saving a loaded profile under another name
preserves its comparison evidence. Explicit settings-only `source-save` clears
cached observations as before. Changing the configured endpoint or channel
starts without a previous observation for that profile selection.

The worker reports a successful profile commit separately from display
allocation failure. Cancellation before saving preserves the profile; a save
that already completed is not reported as undone by a later cancellation.
Filesystem write/durability failures retain the platform service's limitations;
this metadata cache is not a package recovery journal or a power-loss verdict.

## Clients

```text
apkg source-cache ROOT PROFILE ENDPOINT CHANNEL-ID
apkg source-news ROOT PROFILE [--json]
apkg news ROOT ENDPOINT CHANNEL-ID PREVIOUS-PROFILE [--json]
```

`source-news` revalidates the retained pair offline. The explicit `news` command
discovers current metadata and compares the observation held in the specified
previous profile; matching signed channel identity allows mirror endpoints.
Historical CLI spelling/output adaptation remains separate compatibility work.

PkgManager's Last refresh view uses the copied common result, ordered by change
kind and full identity. Search/categories apply to those rows. Details show
prior versions/revisions and publisher dates. Source load restores the pair
after restart, and offline re-evaluation keeps it. Invalid historical evidence
leaves the valid current catalogue readable while Last refresh reports its
error, rather than claiming a first observation or zero changes.

[Implementation status](status.md) distinguishes host checks, native SDK
compilation and the final M1 interaction test. Broader migration between APKG
tool versions remains tracked in [TODO](../../TODO.md); backward reading of one
profile format is not qualification of that whole upgrade path.
