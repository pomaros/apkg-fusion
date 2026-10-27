<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Accepted qualification memory

The root remembers independently authorized signed observations across channels,
mirrors, source omission and cached snapshots. This is local monotonic memory,
not proof of online freshness or a current global approval. Read-only discovery
never commits it. An explicit approved channel acceptance, or a selected channel
installation, retains observations through the existing common journal.

## Stored records and acceptance

An optional root body field `qualification_records` contains the exact `record`
and `signature` pairs described by the [attestation profile](attestations.md).
It is sorted strictly by record SHA-256, contains no duplicates or history forks,
and is bounded to 1024 records plus the context's existing metadata budget.
Missing means empty for earlier common roots. Unknown/malformed records, invalid
signatures and contradictory supplied links refuse. Earlier readers reject the
unknown root field rather than silently erasing accepted evidence.

Acceptance adds only records whose issuer has the required current reviewer or
tester grant and is independent of the exact package authority/signer. Author
publication and channel signatures cannot grant that role or independence.
Author trust and root-target agreement remain separate: remembering evidence
about another target or an untrusted author does not authorize its installation.
An untrusted issuer cannot advance a remembered sequence by claiming a higher
counter. Existing records remain after role/key withdrawal; their current role
and independence facts are re-evaluated when displayed.

Records are never replaced or deleted by ordinary acceptance. An exact repeated
pair is deduplicated; another signature for the same record digest refuses.
Conflicting records at one issuer/kind/content/environment sequence refuse.
Passed, failed and revoked results all remain signed observations. A later valid
report can be remembered without making earlier revocations disappear. Missing
predecessors stay unchecked; continuity is asserted only for the actually
available prefix, with consecutive sequences, exact predecessor digest and
nondecreasing claimed time. Runtime links preserve hardware, OS-build and
suite scope while allowing a new log digest.

Scope identity is SHA-256 over UTF-8 decoded values, without Unicode
normalization: `APKG-ATTESTATION-SCOPE-1` plus LF, then descriptor SHA-256, issuer,
and kind, each followed by LF; runtime additionally includes hardware, OS build
and test suite, each followed by LF. These fields cannot contain LF/NUL/control
bytes, so boundaries are unambiguous. Log bytes are evidence rather than scope.
Alternate JSON escapes cannot change semantic scope. Package names alone cannot
attach remembered evidence to changed content or another target.

## One transaction and retained history

`channel-accept` previews the signed source, replay floor and retained observation
identities/results before confirmation. The shared `APKG_CLIENT_CHANNEL_ACCEPT`
action exposes the same operation to the PkgManager worker; its menu accepts an
endpoint and a channel identity. No archive is transferred and installed records remain byte-for-byte identical.
Metadata-only acceptance does not re-authorize installed publishers, resolve their
dependencies or touch their payloads; withdrawn publisher grants and personal
file edits do not prevent remembering independent evidence. A selected channel
install includes observation retention and the source floor in the same approved
package plan, without downloading unrelated archives.

Root generation changes on acceptance; package generation does not. Direct
install, dependency resolution, upgrade, repair and removal preserve these
records. Package rollback restores package state while preserving current
accepted evidence and source floors. Repeated recovery uses the existing commit
decision: before it, recovery restores the original root; after it, it completes
the same root state. No second qualification journal or recovery model exists.
Over-limit additions refuse without discarding history. Retention/reset/garbage
collection needs a separately approved protocol and is not automatic.

## Discovery and offline projection

The channel owner first preserves supplied observations in wire order, then
appends matching retained pairs absent from that source, sorted by record digest.
Duplicate supplied/retained pairs share one view. Every attached record must bind
the candidate's exact descriptor and identity; signatures and current role facts
are checked again. Forks and signature replacement against remembered evidence
refuse, including when the current source alone would otherwise be valid.

Entry `source_attestation_count` counts supplied records; `attestation_count`
includes the retained projection. Views/JSON distinguish `source`, `retained` and
`source-and-retained` origins. `accepted_sequence`/`accepted_record` identify the
highest previously accepted observation for that exact scope, or are unknown.
`older_than_accepted` marks an earlier record relative to this local memory.
A new unapproved source claim does not replace that reference, even if its
claimed sequence is larger. This is not a latest-head assertion.

The CLI and native worker show a retained revocation when another source omits
it or provides only an old positive report. Source profiles retain original
source bytes; restoring them combines those bytes with the selected root's
current memory and policy without network access or implicit writes. Moving a
profile to another root does not transfer that root's approvals. All objects
remain owned by their existing channel/native models.

## Evidence and remaining gates

Host cases cover HTTP/CLI/actual-worker equivalence, explicit decline/accept,
omitted evidence and old profiles, unauthorized high counters, key withdrawal,
forks/signature replacement, exact-content separation, direct and selected
installation, repair/removal, rollback across later acceptance, process death,
interrupted/repeated offline recovery and allocation failures, including installed
packages, withdrawn publisher grants and edited payloads. Planner/read failures
report NOMEM without turning failed verification allocations into INVALID, stale
history, or unnecessary payload changes. They establish
process-visible host behavior, not native handler/power-loss durability.

Remote revocation discovery and expiry/freshness policy remain required. Local
memory cannot know a revocation it has never observed, and a fresh-looking
source timestamp cannot supply that knowledge. Current qualification requirements,
policy rotation/reset, cross-root migration and portal adaptation remain open.
No current approval, runtime execution or joint-release claim follows from this
memory or from a valid passed signature.
