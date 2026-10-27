<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Source settings and retained observations

A source profile is an explicitly selected client file, separate from managed
root state, policy, journals and the archive/object stores. It holds one endpoint,
one channel identity and optionally the exact channel object/signature observed
there. Multiple profile files can coexist; this is not a completed multi-channel
aggregation or source-precedence model.

Saving settings grants no keys, advances no replay floor and installs nothing.
Settings-only saves clear an older observation. Retaining a channel copies its
exact signed bytes without rewriting them. Endpoint settings themselves are
unsigned: a profile is not evidence that an endpoint is authoritative or current.
The engine stores no private signing keys in a profile.

The common client API reads framing without requiring a root so settings can be
restored even when no initialized root is available. Opening its observation
then verifies the channel and package signatures, current channel/author roles,
root target and durable replay floor against a read-only context. Unknown online
freshness stays unknown. Withdrawn channel trust, old sequences, same-sequence
forks, corrupt signatures and pending roots refuse explicitly. Author withdrawal
and target mismatch remain facts of individual candidates, independent of
whether their versions are newer. There is no network fallback.

Returned snapshot/channel handles own their data independently of the profile
and temporary context. A retained observation carries no transfer origin or
install authorization. An explicit subsequent selected install still uses its
endpoint-bound common plan and revalidates the live source. A cached descriptor
cannot recreate an unavailable archive or imply a completed dependency plan.

## Commands and native controls

```
apkg source-save PROFILE ENDPOINT CHANNEL-ID
apkg source-cache ROOT PROFILE ENDPOINT CHANNEL-ID
apkg source-show PROFILE
apkg source-outdated ROOT PROFILE [FIELD]
```

`source-save` writes settings only. `source-cache` performs read-only discovery
then saves the settings and verified observation. Discovery failure preserves
the previous profile. `source-show` reports settings and whether bytes are
retained, without asserting signature validity. `source-outdated` revalidates and
compares retained metadata with the current root entirely offline, using the
same [update summary](updates.md) as normal discovery and PkgManager.

The native catalogue has a profile path and Save source/cache and Restore
source/cache buttons. Its default path is `ENVARC:apkg-fusion.source`, overridable
with `--source-profile PATH` or the path control. Startup reads an existing
profile without creating one. Settings populate the endpoint/identity controls;
retained observations are opened through a common worker job. Refusal does not
replace an already displayed model. No catalogue is fabricated for settings-only
profiles or unavailable roots.

Save retains the displayed observation only when its complete endpoint and
channel identity match the controls, after revalidation against the current
root. Changing either saves settings without reusing another source's bytes.
Save is explicit; network discovery, browsing, installation and exit do not
silently write the profile. A new source configuration grants no trust.

The worker produces a fresh independent native allocation containing all rows,
summary, provenance and exact selections before handing ownership to the UI.
Cancellation is checked before/after the bounded metadata operation. Native
controls/startup have source/header/type evidence until runtime qualification;
the actual worker/helper paths run in the host suites.

## Local file framing and replacement

This private client file is a byte container rather than a signed common object.
Its header is 41 bytes: a 13-byte magic (`APKG-SOURCE` then `00 01`), a 16-byte
channel identity, then three unsigned big-endian 32-bit lengths for endpoint,
channel object and signature. The three byte spans follow in that order, with no
terminators or trailing bytes. The endpoint is 1–4095 bytes and contains no NUL,
ASCII control or DEL byte. Object and signature must either both be present or
both absent. The whole file is bounded to 16 MiB. Unknown versions, truncation,
length overflow and trailing bytes refuse. Signed object/envelope bytes retain
their existing [canonical/signature framing](serialization.md).

Reads require regular files and refuse final symlinks/FIFOs without blocking.
Explicit saves use the existing platform private-file service: an exclusive
same-directory temporary file starts with owner-only mode, is flushed/closed,
and atomically replaces the destination. Final symlinks refuse. The platform
must support atomic replacement before save starts; old AROS DOS implementations
are unsupported for this write. Concurrent successful saves are last-wins; this
is configuration replacement, not a managed transaction or trust decision.
The host tests establish process-visible replacement and failures, not power-loss
or native handler durability. Invalid inputs and pre-replacement failures keep
the old file; interrupted saves can leave adapter-owned temporary files, never
an accepted partial profile. Automatic cleanup/retention is not implemented.
