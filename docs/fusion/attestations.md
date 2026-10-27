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

Channel transport/publication of these records, qualified row/details/filter
projection, cached history/revocation-head handling and portal adaptation remain
required integration work. Installation policy requiring current review/testing
is unsupported; these observations cannot silently authorize such a policy.
No joint-release approval or target/runtime qualification follows from this
module's host tests.
