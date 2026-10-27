<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 John Knipper -->

# Object and signature framing

CONTRACT revision 1 defines deterministic common-object bytes and signing
transcripts. The [common engine](initial-engine.md) implements this framing;
it does not silently convert existing manifests, signatures or installed
databases. The [reference checker](../../tests/contracts/check.py) produces
framing test vectors; it is not a production parser or verifier.

## Canonical object bytes

Format `apkg-json-1` is a restricted JSON encoding:

- UTF-8 without BOM, final newline or other insignificant whitespace;
- objects with unique UTF-8 string keys sorted by unsigned UTF-8 byte order;
- arrays in declared order; semantic sets are sorted by their specified stable
  identity before encoding and reject duplicates;
- strings contain Unicode scalar values, with no normalization. Quote/backslash
  are escaped as `\"`/`\\`; backspace/tab/LF/form-feed/CR use `\b\t\n\f\r`;
  other U+0000–U+001F use lowercase `\u00xx`; all other scalars are literal UTF-8;
- JSON values are objects, arrays, strings, booleans or null. Numeric tokens,
  NaN/infinity, duplicate keys and lone surrogate code points are invalid.

Every integer is a schema-typed decimal string: unsigned values use `0` or
`[1-9][0-9]*`, maximum `18446744073709551615`. They cannot be negative, padded,
fractional or exponential. Schema-typed text strings are not interpreted as
numbers. IDs are 32 lowercase hex characters for 128-bit values; SHA-256 is
64 lowercase hex characters. Binary/path bytes that are not UTF-8 use a typed
object `{ "encoding": "hex", "value": "..." }`, lowercase even-length hex.
Display text cannot be substituted for raw path bytes during placement.

All common object envelopes have exactly these required fields:
`format` (string `apkg-json-1`), `type` (object family), `schema` (decimal string
`1`), `requires` (sorted unique feature-id strings), and `body` (object).
Optional extensions live inside `body.extensions`; unknown top-level fields
are refused. Type identifiers are `root`, `installed`, `descriptor`, `channel`,
`attestation`, `policy`, `plan`, `result`, `transaction` and `legacy-evidence`.
Object-specific required fields/feature gates follow the data model and are
frozen with their owning implementation batch; the framing envelope alone is
never enough to accept a descriptor for installation.

The initialization seed `policy` body's first supported subset has
`authorities`: an array of objects with `id` (128-bit lowercase hex) and
`keys`: a nonempty array of `{ "algorithm": "ed25519", "public_key": "..." }`.
Authorities sort by id, keys by raw public-key bytes; duplicate ids/keys are
refused. An empty authorities array grants no package trust. Only the implemented role arrays are permitted in this policy subset;
descriptive policy extensions remain unsupported. Each key
authorizes only package signatures for its named authority, not channel/reviewer
privileges or arbitrary system writes. Unsupported policy features are required
features, never ignored fields. Root initialization shows/persists these grants;
subsequent key or role changes need separately approved policy plans.

The [signed-channel checkpoint](channels.md) extends this policy subset with an
optional `channels` grant array of the same sorted id/keys shape. It authorizes
only discovery signatures for the configured channel id. Authority and channel
roles are checked separately, including when the operator explicitly grants
both roles to one identity. The channel body and metadata limits are specified
in that profile; the framing envelope alone is insufficient to accept entries.
Optional signed publication days follow the
[publication news profile](publication-news.md); missing dates stay unknown.

Unknown optional descriptive extension bytes survive unchanged. Unknown
required features refuse execution. Distinct byte spellings of an equivalent
JSON object are not accepted as the same canonical object: parse, validate and
compare canonical encoding with original bytes; reject a mismatch. Preserve
original bytes for diagnostic/legacy purposes, never silently re-encode signed
input and verify its replacement. Legacy formats use their original verifier
and digest domain, not this canonicalization path.

Budgets in [API](api.md#budgets-and-refusal-rules) apply before allocation and
during parsing. Depth starts at 1 for the envelope. Member/element counts are
cumulative across the object. A streaming parser must enforce limits before
materializing hostile data; the small host checker is deliberately not such a
parser. Full decoded schema, path safety and target/authority checks are still
required after framing validity.

## Content and plan identity

SHA-256 hashes the exact canonical object bytes. The typed reference carries
algorithm `sha256`, domain (object family), digest and byte length. File/archive
references instead hash their exact bytes under file/archive domains. Never
compare references across domains merely because digest bytes agree.

Plan bytes include all semantic decisions/preconditions from the data model,
root binding and policy references; exclude presentation-only localized text,
pointers and process timestamps. Ordered effects remain ordered. Relative
paths plus explicit physical-root binding are required; strings naming assigns
alone do not bind a plan. The plan digest covers the complete envelope including
required features. Approval is an in-memory session capability per API, not a
field that can be set to true in a serialized plan. Imported plans require full
validation and a fresh local approval; an importer is outside this tranche.

## Detached signature transcript

Use the existing Ed25519 implementation on the following **83-byte message**,
with SHA-256 over exact object bytes. This signs a domain-separated digest
message using ordinary Ed25519; it is not Ed25519ph.

| Offset | Bytes | Meaning |
|---|---|---|
| 0 | 10 | ASCII `APKG-SIG`, then byte 0, then byte 1 (transcript version) |
| 10 | 1 | Domain: 1 descriptor, 2 channel, 3 attestation |
| 11 | 8 | Unsigned object byte length, big endian |
| 19 | 32 | SHA-256 digest of the canonical object bytes |
| 51 | 32 | Signer's raw Ed25519 public key |

The detached envelope is canonical JSON (without the common object wrapper):
`format = apkg-signature-1`, `domain` naming the domain above, `bytes` as uint64
string, `sha256`, `public_key` (64 lowercase hex), `signature` (128 lowercase
hex). Exactly these six keys are allowed. Reject mismatched length/hash/type,
unknown format/domain, malformed encodings and invalid signatures. A signature
for a channel cannot be accepted as a package signature. Envelopes are separate
from descriptor content, so adding a signature does not change build identity.
Multiple signers are separate envelopes; a future collection wrapper must not
change what each signature covers.

Verification is cryptographic evidence only. Descriptor authority/key binding,
channel policy, attestation issuer privileges, freshness/revocation and target
checks remain independent mandatory decisions. The signer's key is bound into
the transcript but cannot self-grant an existing authority or reviewer role.
Existing pkg manifests/signatures retain their old transcript and explicit
format adapter; no guessed conversion or fallback between formats is allowed.

## Vectors and limits of evidence

[wire-vectors.json](../../tests/fixtures/contracts/wire-vectors.json) supplies a
minimal framing object, its exact bytes/digest and signature transcript using
public-key bytes 00..1f. It is synthetic and has no private key or valid
signature. It is not a complete installable descriptor. Rejection cases cover
noncanonical bytes, duplicates, numeric tokens, invalid Unicode and integer
bounds. The checker verifies framing/domain separation only. SIGNED-READ must
add valid/invalid Ed25519 signatures, full descriptor schema, trust/target and
legacy-format cases before any installation claim.

Independent review/runtime claim bodies and reviewer/tester role grants are
defined by the [attestation profile](attestations.md). Their signatures use
domain tag 3; a signed result is separate from current approval or freshness.

The optional root `qualification_records` array retains exact signed observations
through the common journal. Its strict record ordering, bounds and compatibility
rules are defined in [accepted qualification memory](qualification-memory.md).
