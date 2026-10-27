<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Signed qualification observations

The common engine verifies portable review/runtime claims independently of author
and channel signatures. This is evidence inspection, not a fresh approval gate.
Open publication and installation do not require an attestation. Author metadata
still cannot carry reviewer privileges or an asserted qualification.

Root policy optionally grants `reviewers` and `testers`, using the same sorted
identity/key arrays as authors and channels. Initialization displays each role
and key before approval. Absence grants nothing; an author/channel key never
implicitly gains either role. Current role authorization and signature validity
are separate facts. An unknown or withdrawn issuer can still have a valid signed
claim, shown with rejected role authorization rather than a fabricated approval.

Independence is relative to the exact package authority: issuer identity must
differ, signer key must differ from the descriptor signer, and it must not be a
key currently granted to that package authority. Reusing an author key under a
reviewer alias, granting that key both roles, or using another key of the same
authority cannot satisfy independence. Authorization for review does not grant
runtime-test authorization. Untrusted authors and root-target disagreement are
reported separately; a claim never grants package installation trust.

## Record body

The canonical `apkg-json-1` envelope has type `attestation`, schema `1` and empty
`requires`. Its body has exactly these fields:

| Field | Meaning |
|---|---|
| `authority`, `name`, `version`, `revision`, `target` | Complete descriptor identity, compared exactly, including the packaging revision and qualified target. |
| `descriptor_sha256` | SHA-256 of the exact descriptor bytes, not a name/version match or archive digest. |
| `issuer` | Issuer's 128-bit identity, authorized independently for the chosen role. |
| `kind` | `review` or `runtime`. |
| `result` | Issuer's `passed`, `failed` or `revoked` claim. |
| `sequence` | Nonzero unsigned 64-bit sequence for this issuer/kind/exact descriptor and environment scope history. |
| `issued_at` | Nonzero unsigned Unix-seconds timestamp claimed by the issuer. It is not authenticated wall-clock observation or an expiry policy. |
| `supersedes` | Null for sequence 1; otherwise the preceding attestation's SHA-256. A revocation must have a predecessor. |
| `summary`, `evidence` | Nonempty text, at most 1000/4096 decoded bytes, without ASCII controls or DEL. Evidence is an opaque locator/description; it is not fetched or executed. |
| `environment` | Null for review. Runtime requires exactly `hardware`, `os_build`, `test_suite` (nonempty text, each at most 255 decoded bytes), and `log_sha256` (an exact claimed log digest). |

Integers use canonical decimal strings. Hex identities/digests and qualified
target spelling follow the existing contracts. Unknown fields/features refuse.
The record uses signature domain `attestation` (tag 3), with the existing
[signature transcript](serialization.md). Descriptor and attestation signatures
are both verified; any identity/content mismatch refuses. Document sizes are
bounded to 512 KiB each, signature envelopes to 16 KiB each.

## Ownership, history and freshness

`apkg_attestation_read` uses one captured ready snapshot, exact descriptor and
signature, record and signature, plus an optional preceding attestation owner.
Its output owns all views independently of inputs, the snapshot/context and the
previous owner. Pending/unusable roots refuse. No filesystem/network operation,
evidence retrieval, payload verification or runtime execution occurs in this
API. The result records captured root generation and snapshot digest.

With a supplied predecessor, the engine checks its exact digest, issuer, kind,
descriptor, consecutive sequence and nondecreasing claimed timestamp. Runtime
links also require the same hardware, OS build and test suite; a new log digest
is permitted for a new execution. A wrong
link, fork or cross-identity predecessor refuses. A contiguous observed prefix
starting at sequence 1 is labelled `linked-prefix`. Without a supplied prior
record, a later record remains `not-checked`; missing history is never invented.
Prefix continuity does not assert all historical keys have current authority,
that an observed result is the latest, or that no later revocation exists.
`freshness` stays unknown even for a complete observed prefix and future-dated
signed claims. Passed/failed/revoked are preserved, never reduced to a green
boolean. Individual old claims remain inspectable with their provenance.

## CLI and PkgManager

```
apkg sign-object REVIEWER-KEY attestation attestation.json
apkg verify-object attestation attestation.json attestation.signature.json
apkg attestation-check ROOT CLAIM-FOLDER
```

The folder contains `descriptor.json`, `signature.json`, `attestation.json` and
`attestation.signature.json`. Signing validates the record schema but grants no
role. Generic verification checks framing/signature, while `attestation-check`
also checks the descriptor binding and captured root policy. Its shared formatter
reports the signed claim, author/issuer trust, independence, target, provenance,
history state, environment and unknown freshness. It does not claim to have
executed a runtime test. The folder command inspects one record; it does not
silently infer a predecessor from unrelated local files.

PkgManager's Inspect attestation folder button reads the common signed-folder
input, runs the same inspector in its existing worker, then displays an owned
text copy. No library handle crosses worker exit. Refusal, cancellation and
native allocation failure leave root state unchanged. Native button/dialog
wiring has header/type evidence until runtime qualification; actual worker/helper
inspection and exact CLI answer equivalence run in the host suites.

## Channel carriage and catalogue presentation

Channel entries optionally contain `attestations`, a set of at most 64 objects
with exactly `record` and `signature`. Records use the profile above and the
existing signature envelope. Set order is strictly increasing record SHA-256;
duplicates, competing records with the same issuer/kind/content/environment
scope and sequence, contradictory predecessors, malformed signatures and
cross-content links refuse. Empty/absent sets mean no supplied observations.
Older readers refuse the new field rather than silently discarding evidence.
This is a bounded discovery set, not an implicit limit on separately retained
history; over-limit publications refuse without removing older records.

The unsigned proposal checker `channel-validate JSONFILE` verifies this embedded
cryptography/binding/history without granting root-policy roles. Actual discovery
re-evaluates each record using the captured root's current author/reviewer/tester
roles and target. The entry getter exposes a count and borrowed wire-order claim
views. JSON discovery/updates expose complete signed records, signature envelopes,
trust/independence/target/history/freshness facts and their captured root digest
and generation. Neither these observations nor their order assert current review
approval, current runtime qualification or publication chronology.

```
apkg-channel PACKAGE.pkg --directory CHANNEL --id CHANNEL-ID --sign CHANNEL-KEY \
  --attestation CLAIM-FOLDER [--attestation ANOTHER-CLAIM-FOLDER]
```

Each folder supplies `attestation.json` and `attestation.signature.json`.
Preflight binds them to the archive's exact descriptor, not a substitute supplied
by the folder. The proposed set is checked before creating a new public endpoint.
Republishing the same artifact appends/deduplicates exact observations while
preserving existing history, then publishes a new immutable channel snapshot.
Repeating an addition or publishing without reports does not erase evidence or
advance an unchanged snapshot. A same-record digest with a different signature
refuses; key replacement requires an explicit new history record, never silent
replacement of provenance. Conflicts/refusals preserve the previous current head.
Publication still needs no independent reviewer grant. Self-reports retain their
author provenance and cannot satisfy independence or reviewer/tester authority.
New content or target identity inherits no observations by name/version matching.

PkgManager deep-copies the shared formatted evidence into its bounded native
row model before the worker exits. Review, runtime and revocation observation
filters use explicit counts; they are not approval filters. Details retain every
claim and its separate authorization/independence/result/environment facts.
Offline row rebuilding and source-profile restoration revalidate the retained
signed channel, including its observation sets, without additional requests.
The common channel replay floor applies to the entire signed snapshot.

[Accepted qualification memory](qualification-memory.md) now retains independently
authorized observations through the common journal and projects omitted evidence
across sources. Views distinguish source/retained origin and older-than-accepted
records; supplied wire order is preserved before appended retained pairs.
Remote revocation discovery, expiry/current-freshness policy and portal adaptation
remain required. No current-approval requirement is silently accepted by these
observation APIs. Native row/filter behavior still needs runtime qualification.
